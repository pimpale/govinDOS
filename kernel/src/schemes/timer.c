#include "channel_internal.h"

#include <stdatomic.h>
#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "lapic.h"
#include "scheduler.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "syscall.h"
#include "timer.h"

struct kernel_timer {
  struct kernel_timer *next;
  struct ring *ring;
  uint64_t id;
  uint64_t deadline_ns;
  uint64_t cookie;
};

uint64_t timer_now_ns(void) { return x86_monotonic_ns(); }

// Caller holds s->timer_lock and is running on s's CPU. LAPIC initial-count
// limits merely cause an early checkpoint for distant deadlines.
static void program_local_locked(struct scheduler *s, uint64_t now) {
  uint64_t deadline = s->quantum_deadline_ns;
  if (s->timers_armed != nullptr &&
      (deadline == 0 || s->timers_armed->deadline_ns < deadline)) {
    deadline = s->timers_armed->deadline_ns;
  }
  if (deadline == 0) {
    x86_lapic_timer_stop();
    return;
  }
  uint64_t delta = deadline > now ? deadline - now : 0;
  uint64_t us = delta / 1000 + (delta % 1000 != 0);
  x86_lapic_timer_arm_oneshot(VECTOR_TIMER, us == 0 ? 1 : us);
}

void timer_cpu_dispatch(uint64_t quantum_ns) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&s->timer_lock);
  uint64_t now = timer_now_ns();
  s->quantum_deadline_ns =
      UINT64_MAX - now < quantum_ns ? UINT64_MAX : now + quantum_ns;
  program_local_locked(s, now);
  spinlock_unlock(&s->timer_lock);
}

void timer_cpu_idle(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&s->timer_lock);
  s->quantum_deadline_ns = 0;
  program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);
}

static void timer_retire(struct kernel_timer *timer) {
  atomic_fetch_sub_explicit(&timer->ring->timer_count, 1,
                            memory_order_relaxed);
  free(timer);
}

bool timer_cpu_interrupt(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&s->timer_lock);
  uint64_t now = timer_now_ns();

  while (s->timers_armed != nullptr &&
         s->timers_armed->deadline_ns <= now) {
    struct kernel_timer *timer = s->timers_armed;
    s->timers_armed = timer->next;
    uint32_t index;
    if (channel_post_data(timer->ring, KEV_TIMER, timer->id, timer->cookie,
                          0, &index)) {
      timer_retire(timer);
    } else {
      // CQ-full expiration is durable but no longer a hardware deadline: the
      // next consumption-ack doorbell replays this list.
      timer->next = s->timers_pending;
      s->timers_pending = timer;
    }
  }

  bool quantum_expired = s->quantum_deadline_ns != 0 &&
                         s->quantum_deadline_ns <= now;
  if (quantum_expired)
    s->quantum_deadline_ns = 0;
  program_local_locked(s, now);
  spinlock_unlock(&s->timer_lock);
  return quantum_expired;
}

static bool id_exists(struct ring *ring, uint64_t id) {
  for (size_t cpu = 0; cpu < g_cpu_state_table_len; cpu++) {
    struct scheduler *s = &g_cpu_state_table[cpu].scheduler;
    spinlock_lock(&s->timer_lock);
    bool found = false;
    for (struct kernel_timer *t = s->timers_armed; t != nullptr; t = t->next)
      found |= t->ring == ring && t->id == id;
    for (struct kernel_timer *t = s->timers_pending; t != nullptr;
         t = t->next)
      found |= t->ring == ring && t->id == id;
    spinlock_unlock(&s->timer_lock);
    if (found)
      return true;
  }
  return false;
}

static uint64_t arm_absolute(struct ring *ring, uint64_t id,
                             uint64_t deadline_ns, uint64_t cookie) {
  if (atomic_load_explicit(&ring->timer_count, memory_order_relaxed) >=
          ring->nslots ||
      id_exists(ring, id)) {
    return SYSERR_EXIST;
  }
  struct kernel_timer *timer = calloc(1, sizeof(*timer));
  if (timer == nullptr)
    return SYSERR_NOMEM;
  *timer = (struct kernel_timer){.ring = ring,
                                 .id = id,
                                 .deadline_ns = deadline_ns,
                                 .cookie = cookie};

  // Command execution is pinned to this CPU by the surrounding IRQ-disabled
  // kernel entry, so a newly earlier timer can immediately replace its shot.
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&s->timer_lock);
  struct kernel_timer **link = &s->timers_armed;
  while (*link != nullptr && (*link)->deadline_ns <= deadline_ns)
    link = &(*link)->next;
  timer->next = *link;
  *link = timer;
  atomic_fetch_add_explicit(&ring->timer_count, 1, memory_order_relaxed);
  program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);
  return 0;
}

static uint64_t cancel_timer(struct ring *ring, uint64_t id) {
  struct scheduler *current = &cpu_state_this()->scheduler;
  for (size_t cpu = 0; cpu < g_cpu_state_table_len; cpu++) {
    struct scheduler *s = &g_cpu_state_table[cpu].scheduler;
    spinlock_lock(&s->timer_lock);
    struct kernel_timer **link = &s->timers_armed;
    while (*link != nullptr &&
           ((*link)->ring != ring || (*link)->id != id))
      link = &(*link)->next;
    if (*link != nullptr) {
      struct kernel_timer *timer = *link;
      *link = timer->next;
      timer_retire(timer);
      if (s == current)
        program_local_locked(s, timer_now_ns());
      spinlock_unlock(&s->timer_lock);
      return 0;
    }
    for (struct kernel_timer *t = s->timers_pending; t != nullptr;
         t = t->next) {
      if (t->ring == ring && t->id == id) {
        spinlock_unlock(&s->timer_lock);
        return SYSERR_AGAIN; // deadline already expired
      }
    }
    spinlock_unlock(&s->timer_lock);
  }
  return SYSERR_INVAL;
}

uint64_t timer_exec(struct thread *curr, struct ring *ring,
                    struct ksqe *sqe) {
  (void)curr;
  switch (sqe->op) {
  case KTIMER_NOW:
    sqe->a = timer_now_ns();
    return 0;
  case KTIMER_ARM_ABS:
    return arm_absolute(ring, sqe->a, sqe->b, sqe->c);
  case KTIMER_CANCEL:
    return cancel_timer(ring, sqe->a);
  default:
    return SYSERR_NOSYS;
  }
}

void timer_replay(struct ring *ring) {
  for (size_t cpu = 0; cpu < g_cpu_state_table_len; cpu++) {
    struct scheduler *s = &g_cpu_state_table[cpu].scheduler;
    spinlock_lock(&s->timer_lock);
    struct kernel_timer **link = &s->timers_pending;
    while (*link != nullptr) {
      struct kernel_timer *timer = *link;
      if (timer->ring != ring) {
        link = &timer->next;
        continue;
      }
      uint32_t index;
      if (!channel_post_data(ring, KEV_TIMER, timer->id, timer->cookie, 0,
                             &index)) {
        break; // this ring's CQ is full; preserve the remaining level state
      }
      *link = timer->next;
      timer_retire(timer);
    }
    spinlock_unlock(&s->timer_lock);
  }
}

static void remove_ring_timers(struct kernel_timer **head,
                               struct ring *ring) {
  struct kernel_timer **link = head;
  while (*link != nullptr) {
    struct kernel_timer *timer = *link;
    if (timer->ring != ring) {
      link = &timer->next;
      continue;
    }
    *link = timer->next;
    timer_retire(timer);
  }
}

void timer_endpoint_destroy(struct ring *ring) {
  struct scheduler *current = &cpu_state_this()->scheduler;
  for (size_t cpu = 0; cpu < g_cpu_state_table_len; cpu++) {
    struct scheduler *s = &g_cpu_state_table[cpu].scheduler;
    spinlock_lock(&s->timer_lock);
    remove_ring_timers(&s->timers_armed, ring);
    remove_ring_timers(&s->timers_pending, ring);
    if (s == current)
      program_local_locked(s, timer_now_ns());
    spinlock_unlock(&s->timer_lock);
  }
  asserts(atomic_load_explicit(&ring->timer_count, memory_order_relaxed) == 0,
          "timer: endpoint destroyed with live timers");
}
