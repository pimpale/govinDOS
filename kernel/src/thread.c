#include "thread.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "irq.h"
#include "paging.h"
#include "scheduler.h"
#include "spinlock.h"
#include "stacks.h"
#include "stdlib/stdlib.h"

// Arch hooks. Implemented in archsrc/<arch>/.
//   switch_context: save callee-saved state + SP at *old_sp_out, load new_sp
//                   and pop its callee-saved state; return to whatever lives
//                   at the top of that stack.
//   arch_thread_init_kernel: forge an initial stack frame so the first
//                   switch_context into this thread lands in thread_trampoline
//                   with (entry, arg) in the right argument registers.
//   arch_thread_install: per-cpu setup before resuming the thread — switch
//                   address space if needed, restore FS/TPIDR_EL0, etc.
extern void switch_context(uint64_t *old_sp_out, uint64_t new_sp);
extern void arch_thread_init_kernel(struct thread *t, void (*entry)(void *),
                                    void *arg);
extern void arch_thread_install(struct thread *t);

struct process *g_kernel_process = nullptr;

static _Atomic uint64_t g_next_tid = 1;
static _Atomic uint64_t g_next_pid = 1;

// Scheduler-loop model: every CPU runs threading_cpu_enter() forever on
// its bootstrap stack. Threads only ever switch *to the scheduler* (via
// yield/block/exit); the scheduler switches *into a thread*. No thread
// ever switches directly to another thread, and no lock is held across
// a context switch — each side locks/unlocks locally.

void threading_init(void) {
  asserts(g_kernel_process == nullptr, "threading_init: called twice");
  g_kernel_process = calloc(1, sizeof(*g_kernel_process));
  asserts(g_kernel_process != nullptr, "threading_init: alloc failed");
  g_kernel_process->pid = atomic_fetch_add(&g_next_pid, 1);
  g_kernel_process->as = g_as_kernel;
  g_kernel_process->is_kernel = true;
}

static struct thread *alloc_thread(struct process *proc) {
  struct thread *t = calloc(1, sizeof(*t));
  asserts(t != nullptr, "thread: alloc failed");
  t->tid = atomic_fetch_add(&g_next_tid, 1);
  t->proc = proc;
  t->status = THREAD_RUNNABLE;
  t->stack_top = stacks_alloc_kernel(STACK_TYPE_KERNEL_TASK);
  asserts(t->stack_top != nullptr, "thread: stack alloc failed");
  return t;
}

struct thread *kthread_spawn(void (*entry)(void *), void *arg) {
  struct thread *t = alloc_thread(g_kernel_process);
  arch_thread_init_kernel(t, entry, arg);
  scheduler_enqueue(t);
  return t;
}

// Each of {yield, thread_block, thread_exit} disables IRQs *before* taking
// the scheduler lock so the lock's internal irq_enable on unlock decrements
// the depth without bringing it to 0. That keeps IRQs masked across the
// switch_context — without that an IRQ could fire between unlock and switch
// and re-enter scheduler state.

void yield(void) {
  irq_disable();
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  spinlock_lock(&cs->scheduler.lock);
  struct thread *curr = cs->scheduler.current_thread;
  curr->status = THREAD_RUNNABLE;
  scheduler_push_local_locked(&cs->scheduler, curr);
  spinlock_unlock(&cs->scheduler.lock);

  // Bounce back to the scheduler loop. It will pick next and switch in.
  switch_context(&curr->arch.kernel_rsp, cs->scheduler.sched_rsp);

  // Resumed: scheduler picked us again. Balance the irq_disable above.
  irq_enable();
}

void thread_block(void) {
  irq_disable();
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  spinlock_lock(&cs->scheduler.lock);
  struct thread *curr = cs->scheduler.current_thread;
  curr->status = THREAD_BLOCKED;
  spinlock_unlock(&cs->scheduler.lock);

  switch_context(&curr->arch.kernel_rsp, cs->scheduler.sched_rsp);

  irq_enable();
}

void thread_unblock(struct thread *t) {
  // status read is unsynchronized — fine while there's only one possible
  // unblocker per blocked thread. If we later allow racing unblocks, make
  // status atomic and CAS BLOCKED->RUNNABLE here.
  asserts(t->status == THREAD_BLOCKED, "thread_unblock: not blocked");
  t->status = THREAD_RUNNABLE;
  scheduler_enqueue(t);
}

[[noreturn]] void thread_exit(void) {
  // No matching irq_enable: we never come back. The depth bump we leave
  // behind is consumed by the scheduler loop, which calls irq_enable
  // after its switch_context returns into the next thread / idle path.
  irq_disable();
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];

  spinlock_lock(&cs->scheduler.lock);
  struct thread *curr = cs->scheduler.current_thread;
  curr->status = THREAD_DEAD;
  spinlock_unlock(&cs->scheduler.lock);

  // No saved-RSP slot to update — we are never coming back.
  uint64_t dead_slot;
  switch_context(&dead_slot, cs->scheduler.sched_rsp);
  __builtin_unreachable();
}

// Entry point used by the arch context-switch trampoline. First instruction
// of every freshly-spawned thread. The scheduler enters us with IRQs off
// (depth=1 from its top-of-iteration irq_disable); we balance that here
// before invoking user code.
void thread_trampoline(void (*entry)(void *), void *arg) {
  irq_enable();
  entry(arg);
  thread_exit();
}
