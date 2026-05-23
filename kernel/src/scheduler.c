#include "scheduler.h"

#include <stdatomic.h>

#include "cpu_state.h"
#include "debug.h"
#include "lapic.h"
#include "spinlock.h"
#include "thread.h"

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

struct thread *scheduler_pop_local_locked(struct cpu_state *cs) {
  if (list_thread_ptr_len(cs->scheduler.queue) == 0) {
    return nullptr;
  }
  thread *t;
  list_thread_ptr_pop_front(cs->scheduler.queue, &t);
  return t;
}

void scheduler_push_local_locked(struct cpu_state *cs, struct thread *t) {
  list_thread_ptr_push_back(cs->scheduler.queue, &t);
}

// Round-robin placement counter. Cheap, predictable, ignores load — fine
// as a starting policy. A future revision may consider thread affinity
// (last_cpu), per-CPU queue depth, or idle-CPU preference.
static _Atomic uint64_t g_next_target = 0;

void scheduler_enqueue(struct thread *t) {
  uint64_t target =
      atomic_fetch_add(&g_next_target, 1) % g_cpu_state_table_len;
  struct cpu_state *cs = &g_cpu_state_table[target];

  bool ie = arch_irq_save();
  spinlock_lock(&cs->scheduler.lock);
  scheduler_push_local_locked(cs, t);
  // Read .idle under the lock so we synchronize with the consumer that
  // sets it under the same lock before unlocking and HLT-ing. Whatever
  // we see here is the consumer's pre-HLT state: if true, they're either
  // about to HLT or already HLT'd, and need an IPI to notice our push.
  bool was_idle =
      atomic_load_explicit(&cs->scheduler.idle, memory_order_relaxed);
  spinlock_unlock(&cs->scheduler.lock);

  if (was_idle) {
    x86_lapic_send_fixed((uint8_t)cs->hw_id, VECTOR_RESCHED);
  }
  arch_irq_restore(ie);
}
