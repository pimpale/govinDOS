#include "thread.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "scheduler.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"

// Arch hooks. Implemented in archsrc/<arch>/.
//   switch_context: save callee-saved state + SP at *old_sp_out, load new_sp
//                   and pop its callee-saved state; return to whatever lives
//                   at the top of that stack.
//   arch_thread_init_user / arch_uthread_set_result: trap_frame.h territory;
//                   declared loosely here to keep this file arch-free.
extern void switch_context(uint64_t *old_sp_out, uint64_t new_sp);
extern void arch_thread_init_user(struct thread *t, uint64_t entry,
                                  uint64_t user_stack_top, uint64_t arg);
extern void arch_uthread_set_result(struct thread *t, uint64_t v);

static _Atomic uint64_t g_next_tid = 1;

// Scheduler-loop model: every CPU runs scheduler_loop() forever on its
// bootstrap stack. Threads only ever switch *to the scheduler* (via the
// park calls below); the scheduler switches *into a thread*. No thread
// ever switches directly to another thread, and no lock is held across a
// context switch — each side locks/unlocks locally.

struct thread *uthread_spawn(struct process *proc, uint64_t entry,
                             uint64_t user_stack_top, uint64_t arg) {
  asserts(proc != nullptr && proc->uid != 0,
          "uthread_spawn: needs a user process");
  struct thread *t = calloc(1, sizeof(*t));
  asserts(t != nullptr, "thread: alloc failed");
  t->tid = atomic_fetch_add(&g_next_tid, 1);
  t->proc = proc;
  t->status = THREAD_RUNNABLE;
  arch_thread_init_user(t, entry, user_stack_top, arg);
  scheduler_enqueue(t);
  return t;
}

struct thread *thread_current(void) {
  return cpu_state_this()->scheduler.current_thread;
}

void thread_unblock(struct thread *t) {
  // A blocker sets status=BLOCKED *before* it has finished switching out
  // (its context save completes inside the park path's switch_context).
  // Spin until the scheduler has fully descheduled it — dispatching a
  // thread whose save is still in flight would run one context on two
  // CPUs. The release store of on_cpu=false in scheduler_loop makes the
  // completed save visible to our acquire load.
  while (atomic_load_explicit(&t->on_cpu, memory_order_acquire)) {
    __asm__ volatile("pause");
  }
  // status read is unsynchronized — fine while there's only one possible
  // unblocker per blocked thread (the SPSC waiter-slot rule; a killer
  // unhooks the slot under the umem lock first, so it cannot race the
  // slot's normal waker).
  asserts(t->status == THREAD_BLOCKED, "thread_unblock: not blocked");
  t->status = THREAD_RUNNABLE;
  scheduler_enqueue(t);
}

void thread_deliver_wait_result(struct thread *t, uint64_t v) {
  // The value lands in the saved frame's rax and becomes the syscall
  // return value when the thread irets back to ring 3.
  arch_uthread_set_result(t, v);
}

// ---------------------------------------------------------------------------
// User-thread parking (single-kernel-stack-per-CPU model)
// ---------------------------------------------------------------------------
//
// These run from syscall/interrupt context: hardware (or IA32_FMASK)
// cleared IF and irq_enter set depth to exactly 1 — the same "one pinned
// level" the scheduler loop maintains across its switch.
//
// The switch_context save target is a throwaway: this kernel context is
// dead the moment we leave, because the thread's real state was already
// captured in its TCB frame (or is irrelevant, for exit). The per-CPU
// interrupt stack we're abandoning is reused fresh by the next entry.

[[noreturn]] static void uthread_park(enum thread_status status,
                                      bool requeue) {
  struct cpu_state *cs = cpu_state_this();
  asserts(cs->irq_depth == 1, "uthread_park: not at interrupt depth 1");

  spinlock_lock(&cs->scheduler.lock);
  struct thread *curr = cs->scheduler.current_thread;
  asserts(curr != nullptr && curr->proc->uid != 0,
          "uthread_park: no current user thread");
  curr->status = status;
  if (requeue) {
    // Local requeue only: pushing to another CPU's queue would let it
    // dispatch us before our switch below completes (on_cpu guards
    // wakers, but nothing guards a cross-CPU requeue-and-pop).
    scheduler_push_local_locked(&cs->scheduler, curr);
  }
  spinlock_unlock(&cs->scheduler.lock);

  uint64_t dead_slot;
  switch_context(&dead_slot, cs->scheduler.sched_rsp);
  __builtin_unreachable();
}

[[noreturn]] void uthread_park_exit(void) { uthread_park(THREAD_DEAD, false); }

[[noreturn]] void uthread_park_yield(void) {
  uthread_park(THREAD_RUNNABLE, true);
}

[[noreturn]] void uthread_park_blocked(void) {
  uthread_park(THREAD_BLOCKED, false);
}
