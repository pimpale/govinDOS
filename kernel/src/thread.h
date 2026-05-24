#ifndef thread_h_INCLUDED
#define thread_h_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// archsrc/<arch>/arch_thread.h provides struct arch_thread, the arch-private
// per-thread state (saved kernel SP, FPU/SIMD save area, etc.).
#include "arch_thread.h"

struct address_space;
typedef struct process process;

enum thread_status {
  THREAD_RUNNABLE,
  THREAD_RUNNING,
  THREAD_BLOCKED,
  THREAD_DEAD,
};

typedef struct thread {
  uint64_t tid;
  struct process *proc;       // never null
  enum thread_status status;

  // Top of this thread's own stack. For a kernel thread, this lives in
  // kernel memory and is the stack the thread always runs on; arch code
  // forges its initial frame here and arch.kernel_rsp later tracks where
  // it suspended. For a userspace thread it would live in the process AS
  // and hold the userspace entry frame instead. Allocator depends on
  // thread kind; this field just records the top. Not freed yet.
  void *stack_top;

  // User TLS base. FSBASE on x86_64, TPIDR_EL0 on aarch64. Unused (0) for
  // kernel threads. Saved/restored by arch code on user<->kernel boundary.
  uint64_t user_tls_base;

  // Arch-private state (saved kernel SP, FPU area, ...).
  struct arch_thread arch;
} thread;

// Element type for generic containers that hold non-owning thread references.
// The vec/list templates require a single-identifier type name for token
// pasting, hence the typedef rather than using `thread *` directly.
typedef thread *thread_ptr;

struct process {
  uint64_t pid;
  struct address_space *as;   // == g_as_kernel for the kernel process
  bool is_kernel;
};

// Singleton process every kernel thread belongs to. Allocated by
// threading_init(); never freed.
extern struct process *g_kernel_process;

// One-time global init. Must run on the BSP after g_as_kernel is set and
// cpu_state_table_init has run. Allocates the kernel process singleton.
void threading_init(void);

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
