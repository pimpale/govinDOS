#include "timer.h"

#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "futex.h"
#include "lapic.h"
#include "scheduler.h"
#include "spinlock.h"
#include "thread.h"
#include "timer_queue.h"

// The clock plus the per-CPU deadline machinery (timer-design.md). Each
// CPU owns one spinlock-guarded tree of parked threads' deadlines, keyed
// (absolute deadline, tid). Arming is always local (the park path runs
// on the parking CPU with IRQs off), so expiry never races an insert;
// remote parties only ever *remove* entries (timer_deadline_cancel), and
// removal never makes a tree minimum earlier, so it never reprograms a
// LAPIC and never needs an IPI.

#define TIMER_IRQ_BATCH 64u

uint64_t timer_now_ns(void) { return x86_monotonic_ns(); }

// Caller holds s->timer_lock and is running on s's CPU. LAPIC initial-count
// limits merely cause an early checkpoint for distant deadlines.
static void program_local_locked(struct scheduler *s, uint64_t now) {
  uint64_t deadline = s->quantum_deadline_ns;
  llrb_tdeadline_iter iter;
  struct tdeadline_key first;
  llrb_tdeadline_iter_begin(s->deadlines, &iter);
  if (llrb_tdeadline_iter_next(&iter, &first, nullptr) &&
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

bool timer_deadline_arm(struct thread *t, uint64_t deadline_ns) {
  struct cpu_state *cs = cpu_state_this();
  struct scheduler *s = &cs->scheduler;
  spinlock_lock(&s->timer_lock);
  struct tdeadline_key key = {.deadline_ns = deadline_ns, .tid = t->tid};
  thread_ptr value = t;
  // tids are unique and a thread parks at most once, so a false return is
  // allocation failure, never a duplicate.
  if (!llrb_tdeadline_insert(s->deadlines, &key, &value)) {
    spinlock_unlock(&s->timer_lock);
    return false;
  }
  t->deadline = deadline_ns;
  t->deadline_cpu = (uint32_t)cs->logical_id;
  // Reprogram only when this entry became the earliest obligation.
  llrb_tdeadline_iter iter;
  struct tdeadline_key first;
  llrb_tdeadline_iter_begin(s->deadlines, &iter);
  if (llrb_tdeadline_iter_next(&iter, &first, nullptr) &&
      first.deadline_ns == deadline_ns && first.tid == t->tid &&
      (s->quantum_deadline_ns == 0 || deadline_ns < s->quantum_deadline_ns))
    program_local_locked(s, timer_now_ns());
  spinlock_unlock(&s->timer_lock);
  return true;
}

void timer_deadline_cancel(struct thread *t) {
  if (t->deadline == 0)
    return;
  struct scheduler *s = &g_cpu_state_table[t->deadline_cpu].scheduler;
  spinlock_lock(&s->timer_lock);
  struct tdeadline_key key = {.deadline_ns = t->deadline, .tid = t->tid};
  // Expiry pops its own entries before claiming, so a winner's keyed
  // removal finding nothing is normal — the timer lock serializes the two.
  thread_ptr removed;
  llrb_tdeadline_remove(s->deadlines, &key, &removed);
  spinlock_unlock(&s->timer_lock);
  t->deadline = 0;
}

bool timer_cpu_interrupt(void) {
  struct scheduler *s = &cpu_state_this()->scheduler;
  struct thread *expired[TIMER_IRQ_BATCH];
  uint32_t nexpired = 0;

  spinlock_lock(&s->timer_lock);
  uint64_t now = timer_now_ns();
  // Claim due waiters while their entries are still in the tree: an entry
  // in the tree means no winner has finished cleanup, so the TCB is alive
  // for the CAS. Removal invalidates iterators, and lost claims leave
  // their entries for the winner to remove — both are handled by
  // re-seeking past the last visited key each round.
  struct tdeadline_key seek = {0, 0};
  while (nexpired < TIMER_IRQ_BATCH) {
    llrb_tdeadline_iter iter;
    struct tdeadline_key key;
    thread_ptr t;
    llrb_tdeadline_iter_lower_bound(s->deadlines, &seek, &iter);
    if (!llrb_tdeadline_iter_next(&iter, &key, &t) || key.deadline_ns > now)
      break;
    seek = (struct tdeadline_key){.deadline_ns = key.deadline_ns,
                                  .tid = key.tid + 1};
    if (!futex_try_claim(t))
      continue; // someone else won the thread; they remove this entry
    thread_ptr removed;
    asserts(llrb_tdeadline_remove(s->deadlines, &key, &removed) &&
                removed == t,
            "timer: deadline tree lost expiring entry");
    t->deadline = 0;
    expired[nexpired++] = t;
  }

  bool quantum_expired = s->quantum_deadline_ns != 0 &&
                         s->quantum_deadline_ns <= now;
  if (quantum_expired)
    s->quantum_deadline_ns = 0;
  // Still-due entries left by the batch cap or lost claims produce
  // another near-immediate shot.
  program_local_locked(s, now);
  spinlock_unlock(&s->timer_lock);

  // Finish outside the timer lock: futex-node removal takes a bucket, and
  // the nesting is bucket -> timer everywhere else.
  for (uint32_t i = 0; i < nexpired; i++) {
    futex_expire_claimed(expired[i]);
  }
  return quantum_expired;
}
