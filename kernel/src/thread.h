#ifndef thread_h_INCLUDED
#define thread_h_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// archsrc/<arch>/arch_thread.h provides struct arch_thread, the arch-private
// per-thread state (saved kernel SP, FPU/SIMD save area, etc.).
#include "arch_thread.h"

struct address_space;
struct process;

enum thread_state {
  THREAD_RUNNABLE,
  THREAD_RUNNING,
  THREAD_BLOCKED,
  THREAD_DEAD,
};

struct thread {
  uint64_t tid;
  struct process *proc;       // never null
  enum thread_state state;

  // Intrusive scheduler queue link.
  struct thread *next;
  struct thread *prev;

  // Kernel stack. The top is written into TSS.rsp0 (x86) or the per-cpu
  // kernel-SP slot (aarch64) by arch_thread_install on each schedule.
  void *kernel_stack_top;
  void *kernel_stack_base;

  // User TLS base. FSBASE on x86_64, TPIDR_EL0 on aarch64. Unused (0) for
  // kernel threads. Saved/restored by arch code on user<->kernel boundary.
  uint64_t user_tls_base;

  // Arch-private state (saved kernel SP, FPU area, ...).
  struct arch_thread arch;
};

struct process {
  uint64_t pid;
  struct address_space *as;   // == g_as_kernel for the kernel process
  bool is_kernel;
  size_t n_threads;
};

// Singleton process every kernel thread belongs to. Allocated by
// threading_init(); never freed.
extern struct process *g_kernel_process;

// One-time global init. Must run on the BSP after g_as_kernel is set and
// cpu_state_table_init has run. Allocates the kernel process singleton.
void threading_init(void);

// Per-CPU entry point into the scheduler. Allocates this CPU's idle thread,
// installs it as current, and context-switches onto it. Never returns —
// the bootstrap stack the caller was running on is abandoned.
[[noreturn]] void threading_cpu_enter(void);

// Spawn a kernel thread. Entry is invoked with `arg` as its only argument.
// If `entry` returns, the thread is reaped via thread_exit().
struct thread *kthread_spawn(void (*entry)(void *), void *arg);

// Cooperative yield. If another thread is runnable on the global queue,
// switch to it; otherwise return immediately. Safe to call from any kernel
// context that holds no spinlocks.
void yield(void);

// Mark current thread blocked and yield. Caller must arrange a wake-up via
// thread_unblock() (e.g. an interrupt handler, a waiter on a condition).
void thread_block(void);

// Move a blocked thread back to the ready queue.
void thread_unblock(struct thread *t);

// Voluntarily exit. Marks the current thread dead and switches away. The
// dead thread's stack is leaked for now — a reaper pass will free it later.
[[noreturn]] void thread_exit(void);

// Internal: assembly bootstrap entry point for freshly-spawned threads.
// Receives (entry, arg) in the arch's first two argument registers. Do
// not call directly.
void thread_trampoline(void (*entry)(void *), void *arg);

#endif // thread_h_INCLUDED
