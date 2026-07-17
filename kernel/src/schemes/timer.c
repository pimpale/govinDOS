#include "channel_internal.h"

#include <stdatomic.h>
#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "lapic.h"
#include "scheduler.h"
#include "scheduler_arch.h"
#include "spinlock.h"
#include "syscall.h"
#include "timer.h"

// Timer values live only in their endpoint tree. Each CPU tree is a secondary,
// non-owning index with one ring pointer keyed by that ring's earliest armed
// deadline. A ring is assigned to its creation CPU until waiter-following
// migration is implemented.
static _Atomic uint64_t g_next_timer_ring_id = 1;

#define TIMER_IRQ_BATCH 64u
#define TIMER_ENDPOINT_MAX 1024u

uint64_t timer_now_ns(void) { return x86_monotonic_ns(); }

static struct scheduler *ring_scheduler(const struct ring *ring) {
  return &ring->timer_cpu->scheduler;
}

static bool ring_deadline_key_equal(const struct ring_deadline_key *a,
                                    const struct ring_deadline_key *b) {
  return a->deadline_ns == b->deadline_ns && a->ring_id == b->ring_id;
}

static bool ring_first_timer_locked(const struct ring *ring,
                                    struct timer_key *key, timer *value) {
  llrb_timer_iter iter;
  llrb_timer_iter_begin(ring->timers, &iter);
  return llrb_timer_iter_next(&iter, key, value);
}

static bool cpu_first_ring_locked(const struct scheduler *s,
                                  struct ring_deadline_key *key,
                                  ring_ptr *ring) {
  llrb_ring_deadline_iter iter;
  llrb_ring_deadline_iter_begin(s->timer_rings, &iter);
  return llrb_ring_deadline_iter_next(&iter, key, ring);
}

// Caller holds this ring's assigned CPU timer lock. Insert the replacement
// before removing the old index entry so allocation failure leaves the old
// index intact and lets an arm operation roll back cleanly.
static bool ring_reindex_locked(struct ring *ring) {
  struct scheduler *s = ring_scheduler(ring);
  struct timer_key first_key;
  bool has_timer = ring_first_timer_locked(ring, &first_key, nullptr);
  if (!has_timer) {
    if (ring->timer_indexed) {
      ring_ptr removed;
      asserts(llrb_ring_deadline_remove(s->timer_rings, &ring->timer_cpu_key,
                                        &removed) &&
                  removed == ring,
              "timer: CPU index lost emptying ring");
      ring->timer_indexed = false;
    }
    return true;
  }

  struct ring_deadline_key next = {
      .deadline_ns = first_key.deadline_ns,
      .ring_id = ring->timer_ring_id,
  };
  if (ring->timer_indexed &&
      ring_deadline_key_equal(&ring->timer_cpu_key, &next))
    return true;

  ring_ptr value = ring;
  if (!llrb_ring_deadline_insert(s->timer_rings, &next, &value))
    return false;
  if (ring->timer_indexed) {
    ring_ptr removed;
    asserts(llrb_ring_deadline_remove(s->timer_rings, &ring->timer_cpu_key,
                                      &removed) &&
                removed == ring,
            "timer: CPU index lost changing ring deadline");
  }
  ring->timer_cpu_key = next;
  ring->timer_indexed = true;
  return true;
}

// Caller holds s->timer_lock and is running on s's CPU. LAPIC initial-count
// limits merely cause an early checkpoint for distant deadlines.
static void program_local_locked(struct scheduler *s, uint64_t now) {
  uint64_t deadline = s->quantum_deadline_ns;
  struct ring_deadline_key first;
  if (cpu_first_ring_locked(s, &first, nullptr) &&
      (deadline == 0 || first.deadline_ns < deadline))
    deadline = first.deadline_ns;
  if (deadline == 0) {
    x86_lapic_timer_stop();
    return;
  }
  uint64_t delta = deadline > now ? deadline - now : 0;
  uint64_t us = delta / 1000 + (delta % 1000 != 0);
  x86_lapic_timer_arm_oneshot(VECTOR_TIMER, us == 0 ? 1 : us);
}

uint64_t timer_endpoint_init(struct ring *ring) {
  if (!llrb_timer_new(&ring->timers))
    return SYSERR_NOMEM;
  if (!llrb_timer_event_new(&ring->timer_pending)) {
    llrb_timer_delete(&ring->timers);
    return SYSERR_NOMEM;
  }
  ring->timer_cpu = cpu_state_this();
  ring->timer_ring_id =
      atomic_fetch_add_explicit(&g_next_timer_ring_id, 1,
                                memory_order_relaxed);
  return 0;
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

void timer_cpu_reprogram(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&s->timer_lock);
  program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);
}

bool timer_cpu_interrupt(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  spinlock_lock(&s->timer_lock);
  uint64_t now = timer_now_ns();

  uint32_t expired = 0;
  struct ring_deadline_key ring_key;
  ring_ptr ring;
  while (expired < TIMER_IRQ_BATCH &&
         cpu_first_ring_locked(s, &ring_key, &ring) &&
         ring_key.deadline_ns <= now) {
    struct timer_key timer_key;
    timer value;
    asserts(ring_first_timer_locked(ring, &timer_key, &value) &&
                timer_key.deadline_ns == ring_key.deadline_ns,
            "timer: CPU and ring minima disagree");

    timer removed;
    asserts(llrb_timer_remove(ring->timers, &timer_key, &removed) &&
                removed.id == value.id,
            "timer: endpoint tree lost expiring timer");
    asserts(ring_reindex_locked(ring),
            "timer: CPU index allocation failed during expiration");

    uint32_t index;
    if (!channel_post_data(ring, KEV_TIMER, value.id, value.cookie, 0,
                           &index)) {
      timer_event event = {.id = value.id, .cookie = value.cookie};
      asserts(llrb_timer_event_insert(ring->timer_pending, &timer_key,
                                      &event),
              "timer: pending-event allocation failed");
    }
    expired++;
  }

  bool quantum_expired = s->quantum_deadline_ns != 0 &&
                         s->quantum_deadline_ns <= now;
  if (quantum_expired)
    s->quantum_deadline_ns = 0;
  // A due ring left by the batch cap produces another near-immediate shot.
  program_local_locked(s, now);
  spinlock_unlock(&s->timer_lock);
  return quantum_expired;
}

static bool find_timer_locked(const struct ring *ring, uint64_t id,
                              struct timer_key *key_out) {
  llrb_timer_iter iter;
  llrb_timer_iter_begin(ring->timers, &iter);
  struct timer_key key;
  timer value;
  while (llrb_timer_iter_next(&iter, &key, &value)) {
    if (value.id == id) {
      if (key_out != nullptr)
        *key_out = key;
      return true;
    }
  }
  return false;
}

static bool find_pending_locked(const struct ring *ring, uint64_t id) {
  llrb_timer_event_iter iter;
  llrb_timer_event_iter_begin(ring->timer_pending, &iter);
  timer_event event;
  while (llrb_timer_event_iter_next(&iter, nullptr, &event)) {
    if (event.id == id)
      return true;
  }
  return false;
}

static uint64_t arm_absolute(struct ring *ring, uint64_t id,
                             uint64_t deadline_ns, uint64_t cookie) {
  struct cpu_state *owner = ring->timer_cpu;
  struct scheduler *s = &owner->scheduler;
  struct cpu_state *current = cpu_state_this();
  spinlock_lock(&s->timer_lock);

  size_t count = llrb_timer_len(ring->timers) +
                 llrb_timer_event_len(ring->timer_pending);
  uint32_t limit = ring->nslots < TIMER_ENDPOINT_MAX
                       ? ring->nslots
                       : TIMER_ENDPOINT_MAX;
  if (count >= limit) {
    spinlock_unlock(&s->timer_lock);
    return SYSERR_NOMEM;
  }
  if (find_timer_locked(ring, id, nullptr) ||
      find_pending_locked(ring, id)) {
    spinlock_unlock(&s->timer_lock);
    return SYSERR_EXIST;
  }

  struct timer_key old_first;
  bool had_first = ring_first_timer_locked(ring, &old_first, nullptr);
  struct timer_key key = {.deadline_ns = deadline_ns,
                          .sequence = ring->timer_sequence++};
  timer value = {.id = id, .deadline_ns = deadline_ns, .cookie = cookie};
  if (!llrb_timer_insert(ring->timers, &key, &value)) {
    spinlock_unlock(&s->timer_lock);
    return SYSERR_NOMEM;
  }
  if (!ring_reindex_locked(ring)) {
    timer removed;
    asserts(llrb_timer_remove(ring->timers, &key, &removed),
            "timer: failed arm rollback");
    spinlock_unlock(&s->timer_lock);
    return SYSERR_NOMEM;
  }

  bool became_earlier = !had_first || deadline_ns < old_first.deadline_ns;
  if (owner == current)
    program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);

  if (owner != current && became_earlier)
    scheduler_arch_reprogram_timer(owner->hw_id);
  return 0;
}

static uint64_t cancel_timer(struct ring *ring, uint64_t id) {
  struct cpu_state *owner = ring->timer_cpu;
  struct scheduler *s = &owner->scheduler;
  struct cpu_state *current = cpu_state_this();
  spinlock_lock(&s->timer_lock);

  struct timer_key key;
  if (!find_timer_locked(ring, id, &key)) {
    uint64_t status = find_pending_locked(ring, id) ? SYSERR_AGAIN
                                                    : SYSERR_INVAL;
    spinlock_unlock(&s->timer_lock);
    return status;
  }

  timer removed;
  asserts(llrb_timer_remove(ring->timers, &key, &removed),
          "timer: endpoint tree lost cancelled timer");
  asserts(ring_reindex_locked(ring),
          "timer: CPU index allocation failed during cancellation");
  if (owner == current)
    program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);
  // A remote cancellation may leave an obsolete earlier shot on the owner;
  // it will fire early and harmlessly recompute the new minimum.
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
  struct scheduler *s = ring_scheduler(ring);
  spinlock_lock(&s->timer_lock);
  for (;;) {
    llrb_timer_event_iter iter;
    llrb_timer_event_iter_begin(ring->timer_pending, &iter);
    struct timer_key key;
    timer_event event;
    if (!llrb_timer_event_iter_next(&iter, &key, &event))
      break;
    uint32_t index;
    if (!channel_post_data(ring, KEV_TIMER, event.id, event.cookie, 0,
                           &index))
      break;
    timer_event removed;
    asserts(llrb_timer_event_remove(ring->timer_pending, &key, &removed),
            "timer: pending tree lost replayed event");
  }
  spinlock_unlock(&s->timer_lock);
}

void timer_endpoint_destroy(struct ring *ring) {
  struct cpu_state *owner = ring->timer_cpu;
  struct scheduler *s = &owner->scheduler;
  bool local = owner == cpu_state_this();
  spinlock_lock(&s->timer_lock);
  if (ring->timer_indexed) {
    ring_ptr removed;
    asserts(llrb_ring_deadline_remove(s->timer_rings, &ring->timer_cpu_key,
                                      &removed) &&
                removed == ring,
            "timer: CPU index lost destroyed ring");
    ring->timer_indexed = false;
  }
  llrb_timer_delete(&ring->timers);
  llrb_timer_event_delete(&ring->timer_pending);
  if (local)
    program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);
}
