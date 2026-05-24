#include "scheduler.h"

#include <stdatomic.h>

#include "cpu_state.h"
#include "debug.h"
#include "irq.h"
#include "scheduler_arch.h"
#include "spinlock.h"
#include "thread.h"

// Implemented in archsrc/<arch>/. Declared here because scheduler_loop
// drives the per-CPU dispatch and needs both.
extern void switch_context(uint64_t *old_sp_out, uint64_t new_sp);
extern void arch_thread_install(struct thread *t);

void scheduler_init(void) {
  cpu_state_table_require();
  for (size_t i = 0; i < g_cpu_state_table_len; i++) {
    struct scheduler *s = &g_cpu_state_table[i].scheduler;
    list_thread_ptr_new(&s->queue);
    spinlock_init(&s->lock);
    s->sched_rsp = 0;
    atomic_store_explicit(&s->idle, false, memory_order_relaxed);
  }
}

struct thread *scheduler_pop_local_locked(struct scheduler *scheduler) {
  if (list_thread_ptr_len(scheduler->queue) == 0) {
    return nullptr;
  }
  thread *t;
  list_thread_ptr_pop_front(scheduler->queue, &t);
  return t;
}

void scheduler_push_local_locked(struct scheduler *scheduler,
                                 struct thread *t) {
  list_thread_ptr_push_back(scheduler->queue, &t);
}

// Round-robin placement counter. Cheap, predictable, ignores load — fine
// as a starting policy. A future revision may consider thread affinity
// (last_cpu), per-CPU queue depth, or idle-CPU preference.
static _Atomic uint64_t g_next_target = 0;

void scheduler_enqueue(struct thread *t) {
  uint64_t target = atomic_fetch_add(&g_next_target, 1) % g_cpu_state_table_len;
  struct cpu_state *cs = &g_cpu_state_table[target];

  spinlock_lock(&cs->scheduler.lock);
  scheduler_push_local_locked(&cs->scheduler, t);
  // Read .idle under the lock so we synchronize with the consumer that
  // sets it under the same lock before unlocking and HLT-ing. Whatever
  // we see here is the consumer's pre-HLT state: if true, they're either
  // about to HLT or already HLT'd, and need an IPI to notice our push.
  bool was_idle =
      atomic_load_explicit(&cs->scheduler.idle, memory_order_relaxed);
  spinlock_unlock(&cs->scheduler.lock);

  if (was_idle) {
    scheduler_arch_wake_cpu(cs->hw_id);
  }
}

[[noreturn]] void scheduler_loop(struct scheduler *scheduler) {
  asserts(g_kernel_process != nullptr,
          "threading_cpu_enter: threading_init not called");
  while (true) {
    // Top of iteration: claim an extra IRQ-disable so the depth stays
    // pinned >0 across the inner unlock and the subsequent switch into
    // the chosen thread (we must not run with IRQs on between unlock and
    // switch_context, or a handler could fire and mutate scheduler state
    // mid-switch).
    irq_disable();
    spinlock_lock(&scheduler->lock);
    struct thread *next = scheduler_pop_local_locked(scheduler);

    if (next == nullptr) {
      // idle if there is no thread to run.
      // Mark ourselves idle BEFORE releasing the lock. A producer that
      // grabs the lock next sees idle=true and will send us the
      // reschedule IPI after its push. Setting under the lock is what
      // makes the "push then check idle" sequence on the producer side
      // race-free against this point.
      atomic_store_explicit(&scheduler->idle, true, memory_order_relaxed);
      spinlock_unlock(&scheduler->lock);
      // Balance the top-of-iteration irq_disable: hlt needs IRQs on to
      // wake from anything.
      irq_enable();

      // halt
      scheduler_arch_idle();

      // Clearing outside the lock is fine: we're the only writer for
      // this CPU's idle flag. A producer racing here may briefly send a
      // spurious IPI (handler is a no-op + EOI), which is harmless.
      atomic_store_explicit(&scheduler->idle, false, memory_order_relaxed);
    } else {
      next->status = THREAD_RUNNING;
      scheduler->current_thread = next;
      arch_thread_install(next);
      spinlock_unlock(&scheduler->lock);

      // Hand control to next thread with IRQs still off (depth=1 from
      // the top-of-iteration irq_disable). The thread side balances its
      // own halves of the switch.
      switch_context(&scheduler->sched_rsp, next->arch.kernel_rsp);

      // Resumed: thread bounced back, depth still 1. Balance now.
      irq_enable();
      scheduler->current_thread = nullptr;
    }
  }
}
