#include "thread.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "paging.h"
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
//   arch_thread_install: per-cpu setup before resuming the thread — write
//                   its kernel_stack_top into the TSS (x86) or per-cpu
//                   kernel-SP slot (aarch64), restore FS/TPIDR_EL0, etc.
extern void switch_context(uint64_t *old_sp_out, uint64_t new_sp);
extern void arch_thread_init_kernel(struct thread *t,
                                    void (*entry)(void *),
                                    void *arg);
extern void arch_thread_install(struct thread *t);

struct process *g_kernel_process = nullptr;

static _Atomic uint64_t g_next_tid = 1;
static _Atomic uint64_t g_next_pid = 1;

// Single global ready queue. Per-CPU queues are a later optimization.
//
// Locking discipline for context switches:
//   - Acquire g_sched.lock with interrupts disabled.
//   - Pick next thread, update current_thread, call switch_context.
//   - The thread we switch *into* is responsible for releasing the lock
//     and re-enabling interrupts. For continuing threads that's the code
//     after switch_context returns; for freshly-spawned threads it's the
//     first thing thread_trampoline does.
static struct {
  struct thread *head;
  struct thread *tail;
  struct spinlock lock;
} g_sched = {.lock = SPINLOCK_INITIALIZER};

static void ready_push_tail(struct thread *t) {
  t->next = nullptr;
  t->prev = g_sched.tail;
  if (g_sched.tail != nullptr) {
    g_sched.tail->next = t;
  } else {
    g_sched.head = t;
  }
  g_sched.tail = t;
}

static struct thread *ready_pop_head(void) {
  struct thread *t = g_sched.head;
  if (t == nullptr) {
    return nullptr;
  }
  g_sched.head = t->next;
  if (g_sched.head != nullptr) {
    g_sched.head->prev = nullptr;
  } else {
    g_sched.tail = nullptr;
  }
  t->next = nullptr;
  t->prev = nullptr;
  return t;
}

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
  t->state = THREAD_RUNNABLE;
  t->kernel_stack_top = stacks_alloc_kernel(STACK_TYPE_KERNEL_TASK);
  asserts(t->kernel_stack_top != nullptr, "thread: stack alloc failed");
  // stacks_alloc_kernel returns the top; the base isn't reported back, so
  // we cannot free the stack yet. Tracked by a future cleanup pass.
  t->kernel_stack_base = nullptr;
  proc->n_threads++;
  return t;
}

struct thread *kthread_spawn(void (*entry)(void *), void *arg) {
  struct thread *t = alloc_thread(g_kernel_process);
  arch_thread_init_kernel(t, entry, arg);

  // Cross-cpu enqueue: any cpu may pick this up.
  bool ie = arch_irq_save();
  spinlock_lock(&g_sched.lock);
  ready_push_tail(t);
  spinlock_unlock(&g_sched.lock);
  arch_irq_restore(ie);
  return t;
}

// Idle body: re-enable interrupts, halt, and yield so any newly enqueued
// thread on this CPU gets a turn.
static void idle_main(void *unused) {
  (void)unused;
  for (;;) {
    arch_irq_enable();
    __asm__ volatile("hlt");
    yield();
  }
}

// Pick the next thread to run on this CPU. Falls back to this CPU's idle
// thread when the global queue is empty. The current thread is requeued
// (unless it is the idle thread or has already moved to a non-running state).
// Caller must hold g_sched.lock and have interrupts disabled.
static struct thread *pick_next_locked(struct cpu_state *cs,
                                       struct thread *curr) {
  struct thread *next = ready_pop_head();
  if (next == nullptr) {
    next = cs->idle_thread;
  }
  if (curr != cs->idle_thread && curr->state == THREAD_RUNNING) {
    curr->state = THREAD_RUNNABLE;
    ready_push_tail(curr);
  }
  return next;
}

void yield(void) {
  bool ie = arch_irq_save();
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  struct thread *curr = cs->current_thread;

  spinlock_lock(&g_sched.lock);
  struct thread *next = pick_next_locked(cs, curr);

  if (next == curr) {
    // Queue empty and we're already idle, or we're the only runnable
    // thread on this CPU. Nothing to switch to.
    spinlock_unlock(&g_sched.lock);
    arch_irq_restore(ie);
    return;
  }

  next->state = THREAD_RUNNING;
  cs->current_thread = next;
  arch_thread_install(next);

  // Hand the still-locked g_sched.lock off to `next`. It will release
  // immediately after switch_context returns into it. When we are later
  // switched back to, we wake up holding it again and release below.
  switch_context(&curr->arch.kernel_rsp, next->arch.kernel_rsp);

  spinlock_unlock(&g_sched.lock);
  arch_irq_restore(ie);
}

void thread_block(void) {
  bool ie = arch_irq_save();
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  struct thread *curr = cs->current_thread;
  asserts(curr != cs->idle_thread, "thread_block: idle thread can't block");

  spinlock_lock(&g_sched.lock);
  curr->state = THREAD_BLOCKED;
  struct thread *next = ready_pop_head();
  if (next == nullptr) {
    next = cs->idle_thread;
  }
  next->state = THREAD_RUNNING;
  cs->current_thread = next;
  arch_thread_install(next);

  switch_context(&curr->arch.kernel_rsp, next->arch.kernel_rsp);

  spinlock_unlock(&g_sched.lock);
  arch_irq_restore(ie);
}

void thread_unblock(struct thread *t) {
  bool ie = arch_irq_save();
  spinlock_lock(&g_sched.lock);
  asserts(t->state == THREAD_BLOCKED, "thread_unblock: not blocked");
  t->state = THREAD_RUNNABLE;
  ready_push_tail(t);
  spinlock_unlock(&g_sched.lock);
  arch_irq_restore(ie);
}

[[noreturn]] void thread_exit(void) {
  arch_irq_disable();
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  struct thread *curr = cs->current_thread;
  asserts(curr != cs->idle_thread, "thread_exit: idle thread can't exit");

  spinlock_lock(&g_sched.lock);
  curr->state = THREAD_DEAD;
  struct thread *next = ready_pop_head();
  if (next == nullptr) {
    next = cs->idle_thread;
  }
  next->state = THREAD_RUNNING;
  cs->current_thread = next;
  arch_thread_install(next);

  // No saved-RSP slot to update — we are never coming back.
  uint64_t dead_slot;
  switch_context(&dead_slot, next->arch.kernel_rsp);
  __builtin_unreachable();
}

// Entry point used by the arch context-switch trampoline. First instruction
// of every freshly-spawned thread.
void thread_trampoline(void (*entry)(void *), void *arg) {
  // We were switched into with g_sched.lock held and interrupts off
  // (the invariant set up by yield / thread_block / threading_cpu_enter).
  // Release and re-enable before running user code.
  spinlock_unlock(&g_sched.lock);
  arch_irq_enable();

  entry(arg);
  thread_exit();
}

[[noreturn]] void threading_cpu_enter(void) {
  asserts(g_kernel_process != nullptr,
          "threading_cpu_enter: threading_init not called");
  arch_irq_disable();

  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  struct thread *idle = alloc_thread(g_kernel_process);
  arch_thread_init_kernel(idle, idle_main, nullptr);
  idle->state = THREAD_RUNNING;
  cs->idle_thread = idle;
  cs->current_thread = idle;
  arch_thread_install(idle);

  // Acquire the lock so that the trampoline's invariant ("we hold the lock
  // and IRQs are off when we're switched in") is satisfied for idle's
  // first run. Released inside thread_trampoline.
  spinlock_lock(&g_sched.lock);

  // Discard the bootstrap stack we are running on. The "old SP out" slot
  // is a throwaway: we never come back.
  uint64_t dead_slot;
  switch_context(&dead_slot, idle->arch.kernel_rsp);
  __builtin_unreachable();
}
