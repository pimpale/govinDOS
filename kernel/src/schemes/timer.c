#include "channel_internal.h"

#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "lapic.h"
#include "scheduler.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "syscall.h"
#include "timer.h"

enum timer_state { TIMER_ARMED, TIMER_PENDING };

struct kernel_timer {
  // Per-CPU hardware queue index. Armed timers only.
  struct scheduler *owner;
  struct timer_deadline_key key;

  // Deadline-sorted endpoint list. Contains armed timers plus expirations
  // waiting for CQ space, so lookup and teardown inspect at most nslots.
  struct kernel_timer *ring_prev;
  struct kernel_timer *ring_next;
  struct ring *ring;

  uint64_t id;
  uint64_t deadline_ns;
  uint64_t cookie;
  enum timer_state state;
};

// One lock owns every timer queue, endpoint list, and quantum deadline. Timer
// operations are short and infrequent; the single lock makes endpoint removal
// O(1), keeps the IRQ path independent of g_umem, and gives one lock order:
// g_umem (commands/teardown only) -> timer -> ring stripe -> scheduler.
static struct spinlock g_timer_lock = SPINLOCK_INITIALIZER;
static uint64_t g_timer_sequence;

#define TIMER_IRQ_BATCH 64u
#define TIMER_ENDPOINT_MAX 1024u

uint64_t timer_now_ns(void) { return x86_monotonic_ns(); }

// Caller holds g_timer_lock and is running on s's CPU. LAPIC initial-count
// limits merely cause an early checkpoint for distant deadlines.
static void program_local_locked(struct scheduler *s, uint64_t now) {
  uint64_t deadline = s->quantum_deadline_ns;
  llrb_timer_deadline_iter iter;
  kernel_timer_ptr first;
  llrb_timer_deadline_iter_begin(s->timers_armed, &iter);
  if (llrb_timer_deadline_iter_next(&iter, nullptr, &first) &&
      (deadline == 0 || first->deadline_ns < deadline))
    deadline = first->deadline_ns;
  if (deadline == 0) {
    x86_lapic_timer_stop();
    return;
  }
  uint64_t delta = deadline > now ? deadline - now : 0;
  uint64_t us = delta / 1000 + (delta % 1000 != 0);
  x86_lapic_timer_arm_oneshot(VECTOR_TIMER, us == 0 ? 1 : us);
}

static struct kernel_timer *cpu_first_locked(struct scheduler *s) {
  llrb_timer_deadline_iter iter;
  kernel_timer_ptr timer;
  llrb_timer_deadline_iter_begin(s->timers_armed, &iter);
  if (!llrb_timer_deadline_iter_next(&iter, nullptr, &timer))
    return nullptr;
  return timer;
}

static void cpu_remove_locked(struct kernel_timer *timer) {
  kernel_timer_ptr removed;
  asserts(llrb_timer_deadline_remove(timer->owner->timers_armed, &timer->key,
                                     &removed) &&
              removed == timer,
          "timer: deadline tree lost armed timer");
}

static void ring_insert_locked(struct kernel_timer *timer) {
  struct kernel_timer **link = &timer->ring->timers;
  struct kernel_timer *prev = nullptr;
  while (*link != nullptr && (*link)->deadline_ns <= timer->deadline_ns) {
    prev = *link;
    link = &(*link)->ring_next;
  }
  timer->ring_prev = prev;
  timer->ring_next = *link;
  if (*link != nullptr)
    (*link)->ring_prev = timer;
  *link = timer;
  timer->ring->timer_count++;
}

static void ring_remove_locked(struct kernel_timer *timer) {
  if (timer->ring_prev != nullptr)
    timer->ring_prev->ring_next = timer->ring_next;
  else
    timer->ring->timers = timer->ring_next;
  if (timer->ring_next != nullptr)
    timer->ring_next->ring_prev = timer->ring_prev;
  timer->ring->timer_count--;
  timer->ring_prev = nullptr;
  timer->ring_next = nullptr;
}

static void timer_retire_locked(struct kernel_timer *timer) {
  ring_remove_locked(timer);
  free(timer);
}

void timer_cpu_dispatch(uint64_t quantum_ns) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&g_timer_lock);
  uint64_t now = timer_now_ns();
  s->quantum_deadline_ns =
      UINT64_MAX - now < quantum_ns ? UINT64_MAX : now + quantum_ns;
  program_local_locked(s, now);
  spinlock_unlock(&g_timer_lock);
}

void timer_cpu_idle(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&g_timer_lock);
  s->quantum_deadline_ns = 0;
  program_local_locked(s, timer_now_ns());
  spinlock_unlock(&g_timer_lock);
}

bool timer_cpu_interrupt(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&g_timer_lock);
  uint64_t now = timer_now_ns();

  uint32_t expired = 0;
  struct kernel_timer *timer;
  while ((timer = cpu_first_locked(s)) != nullptr &&
         timer->deadline_ns <= now && expired < TIMER_IRQ_BATCH) {
    cpu_remove_locked(timer);
    uint32_t index;
    if (channel_post_data(timer->ring, KEV_TIMER, timer->id, timer->cookie,
                          0, &index)) {
      timer_retire_locked(timer);
    } else {
      // CQ-full expiration is durable but leaves the hardware queue. The next
      // consumption-ack doorbell finds it in the endpoint list and replays it.
      timer->state = TIMER_PENDING;
    }
    expired++;
  }

  bool quantum_expired = s->quantum_deadline_ns != 0 &&
                         s->quantum_deadline_ns <= now;
  if (quantum_expired)
    s->quantum_deadline_ns = 0;
  // A due head left by the batch cap produces another near-immediate shot.
  program_local_locked(s, now);
  spinlock_unlock(&g_timer_lock);
  return quantum_expired;
}

static struct kernel_timer *find_timer_locked(struct ring *ring, uint64_t id) {
  for (struct kernel_timer *timer = ring->timers; timer != nullptr;
       timer = timer->ring_next) {
    if (timer->id == id)
      return timer;
  }
  return nullptr;
}

static uint64_t arm_absolute(struct ring *ring, uint64_t id,
                             uint64_t deadline_ns, uint64_t cookie) {
  struct kernel_timer *timer = calloc(1, sizeof(*timer));
  if (timer == nullptr)
    return SYSERR_NOMEM;
  *timer = (struct kernel_timer){.owner = &cpu_state_this()->scheduler,
                                 .ring = ring,
                                 .id = id,
                                 .deadline_ns = deadline_ns,
                                 .cookie = cookie,
                                 .state = TIMER_ARMED};

  spinlock_lock(&g_timer_lock);
  uint32_t limit = ring->nslots < TIMER_ENDPOINT_MAX
                       ? ring->nslots
                       : TIMER_ENDPOINT_MAX;
  if (ring->timer_count >= limit) {
    spinlock_unlock(&g_timer_lock);
    free(timer);
    return SYSERR_NOMEM;
  }
  if (find_timer_locked(ring, id) != nullptr) {
    spinlock_unlock(&g_timer_lock);
    free(timer);
    return SYSERR_EXIST;
  }
  timer->key = (struct timer_deadline_key){.deadline_ns = deadline_ns,
                                          .sequence = g_timer_sequence++};
  if (!llrb_timer_deadline_insert(timer->owner->timers_armed, &timer->key,
                                  &timer)) {
    spinlock_unlock(&g_timer_lock);
    free(timer);
    return SYSERR_NOMEM;
  }
  ring_insert_locked(timer);
  program_local_locked(timer->owner, timer_now_ns());
  spinlock_unlock(&g_timer_lock);
  return 0;
}

static uint64_t cancel_timer(struct ring *ring, uint64_t id) {
  struct scheduler *current = &cpu_state_this()->scheduler;
  spinlock_lock(&g_timer_lock);
  struct kernel_timer *timer = find_timer_locked(ring, id);
  if (timer == nullptr) {
    spinlock_unlock(&g_timer_lock);
    return SYSERR_INVAL;
  }
  if (timer->state == TIMER_PENDING) {
    spinlock_unlock(&g_timer_lock);
    return SYSERR_AGAIN;
  }
  cpu_remove_locked(timer);
  struct scheduler *owner = timer->owner;
  timer_retire_locked(timer);
  if (owner == current)
    program_local_locked(owner, timer_now_ns());
  spinlock_unlock(&g_timer_lock);
  return 0;
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
  spinlock_lock(&g_timer_lock);
  struct kernel_timer *timer = ring->timers;
  while (timer != nullptr) {
    struct kernel_timer *next = timer->ring_next;
    if (timer->state == TIMER_PENDING) {
      uint32_t index;
      if (!channel_post_data(ring, KEV_TIMER, timer->id, timer->cookie, 0,
                             &index))
        break;
      timer_retire_locked(timer);
    }
    timer = next;
  }
  spinlock_unlock(&g_timer_lock);
}

void timer_endpoint_destroy(struct ring *ring) {
  struct scheduler *current = &cpu_state_this()->scheduler;
  bool reprogram_current = false;
  spinlock_lock(&g_timer_lock);
  while (ring->timers != nullptr) {
    struct kernel_timer *timer = ring->timers;
    if (timer->state == TIMER_ARMED) {
      cpu_remove_locked(timer);
      reprogram_current |= timer->owner == current;
    }
    timer_retire_locked(timer);
  }
  if (reprogram_current)
    program_local_locked(current, timer_now_ns());
  spinlock_unlock(&g_timer_lock);
  asserts(ring->timer_count == 0,
          "timer: endpoint destroyed with live timers");
}
