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
// User-thread arch hooks live in trap_frame.h territory; declared loosely
// here to keep this file free of arch includes.
extern void arch_thread_init_user(struct thread *t, uint64_t entry,
                                  uint64_t user_stack_top);
extern void arch_uthread_set_result(struct thread *t, uint64_t v);

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
  g_kernel_process->uid = 0;
}

struct process *process_create_user(uint64_t uid) {
  struct process *p = calloc(1, sizeof(*p));
  asserts(p != nullptr, "process: alloc failed");
  p->pid = atomic_fetch_add(&g_next_pid, 1);
  p->as = g_as_kernel; // single address space: protection is PAGE_U, not CR3
  p->is_kernel = false;
  p->uid = uid;
  return p;
}

// User threads are stackless in the kernel (their state lives in the TCB
// trap frame), so only kernel threads get the big per-thread stack.
static struct thread *alloc_thread(struct process *proc, bool with_kstack) {
  struct thread *t = calloc(1, sizeof(*t));
  asserts(t != nullptr, "thread: alloc failed");
  t->tid = atomic_fetch_add(&g_next_tid, 1);
  t->proc = proc;
  t->status = THREAD_RUNNABLE;
  if (with_kstack) {
    t->stack_top = stacks_alloc_kernel(STACK_TYPE_KERNEL_TASK);
    asserts(t->stack_top != nullptr, "thread: stack alloc failed");
  }
  return t;
}

struct thread *kthread_spawn(void (*entry)(void *), void *arg) {
  struct thread *t = alloc_thread(g_kernel_process, true);
  arch_thread_init_kernel(t, entry, arg);
  scheduler_enqueue(t);
  return t;
}

struct thread *uthread_spawn(struct process *proc, uint64_t entry,
                             uint64_t user_stack_top) {
  asserts(proc != nullptr && !proc->is_kernel,
          "uthread_spawn: needs a user process");
  struct thread *t = alloc_thread(proc, false);
  arch_thread_init_user(t, entry, user_stack_top);
  scheduler_enqueue(t);
  return t;
}

struct thread *thread_current(void) {
  return cpu_state_this()->scheduler.current_thread;
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
  // A blocker sets status=BLOCKED *before* it has finished switching out
  // (its context save completes inside switch_context / the park path).
  // Spin until the scheduler has fully descheduled it — dispatching a
  // thread whose save is still in flight would run one context on two
  // CPUs. The release store of on_cpu=false in scheduler_loop makes the
  // completed save visible to our acquire load.
  while (atomic_load_explicit(&t->on_cpu, memory_order_acquire)) {
    __asm__ volatile("pause");
  }
  // status read is unsynchronized — fine while there's only one possible
  // unblocker per blocked thread. If we later allow racing unblocks, make
  // status atomic and CAS BLOCKED->RUNNABLE here.
  asserts(t->status == THREAD_BLOCKED, "thread_unblock: not blocked");
  t->status = THREAD_RUNNABLE;
  scheduler_enqueue(t);
}

void thread_deliver_wait_result(struct thread *t, uint64_t v) {
  if (t->proc->is_kernel) {
    t->wait_result = v;
  } else {
    // Parked user thread: the value lands in the saved frame's rax and
    // becomes the syscall return value when it irets back to ring 3.
    arch_uthread_set_result(t, v);
  }
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
void thread_enter(void (*entry)(void *), void *arg) {
  irq_enable();
  entry(arg);
  thread_exit();
}

// ---------------------------------------------------------------------------
// User-thread parking (single-kernel-stack-per-CPU model)
// ---------------------------------------------------------------------------
//
// These run from syscall/interrupt context: hardware cleared IF and
// irq_enter set depth to exactly 1 — the same "one pinned level" the
// scheduler loop maintains across its switch, so no extra irq_disable
// here (contrast yield/thread_block above, which run at depth 0).
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
  asserts(curr != nullptr && !curr->proc->is_kernel,
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
