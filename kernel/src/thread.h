#ifndef thread_h_INCLUDED
#define thread_h_INCLUDED

#include <stdatomic.h>
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

  // True from just before the scheduler switches into this thread until
  // just after it has switched back out and its saved state is complete.
  // Wakers must spin this false before re-enqueueing (thread_unblock does)
  // — otherwise a thread that has set status=BLOCKED but not yet finished
  // saving its context could be dispatched on another CPU.
  _Atomic bool on_cpu;

  // Scratch slot for wake-up payloads delivered to a blocked *kernel*
  // thread (a parked user thread receives results in its saved trap frame
  // instead — see thread_deliver_wait_result).
  uint64_t wait_result;

  // This thread's submission/completion ring, or nullptr. Rings are
  // per-thread (SPSC with the ring's worker kthread). Owned here; created
  // on first SYS_RING_CREATE. See ring.h.
  struct ring *ring;

  // Arch-private state (saved kernel SP, FPU area, ...).
  struct arch_thread arch;
} thread;

// Element type for generic containers that hold non-owning thread references.
// The vec/list templates require a single-identifier type name for token
// pasting, hence the typedef rather than using `thread *` directly.
typedef thread *thread_ptr;

struct process {
  uint64_t pid;
  // The kernel process runs on g_as_kernel; every user process gets its
  // own as_clone(g_as_template). All ASes share the identity layout
  // (SASOS) — isolation is which tree carries PAGE_U leaves for a block.
  struct address_space *as;
  // Owner of this process, for the multi-user model. uid 0 is the kernel
  // process. User memory is charged to this uid (umem.c accounts).
  uint64_t uid;

  // Live threads belonging to this process. Incremented at spawn,
  // decremented by the reaper after a dead thread's teardown; the reaper
  // destroys the process when it hits zero (kernel process excepted).
  _Atomic uint64_t nthreads;

  // User memory (umem.c, guarded by its lock): blocks this process owns,
  // and blocks other processes have shared with it.
  struct vec_ublock_ptr *blocks;
  struct vec_ublock_ptr *shared_in;

  // Session/IPC state (session.c): listener consent + the sessions this
  // process participates in. Allocated for user processes at creation, torn
  // down first in process death. nullptr for the kernel process.
  struct proc_ipc *ipc;
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
void thread_enter(void (*entry)(void *), void *arg);

// ---------------------------------------------------------------------------
// User threads & processes
// ---------------------------------------------------------------------------
//
// User threads are stackless in the kernel: their suspended state is the
// trap frame saved in the TCB by arch_uthread_save_frame, and they are
// resumed through a forged frame that irets straight back to ring 3. A
// user thread may therefore only park at a point where its entire kernel
// state is that frame — i.e. from the syscall/interrupt path, after the
// frame has been saved. That is what keeps the single-kernel-stack-per-CPU
// model sound, including under future preemption.

// Create a user process. Same identity address layout as everything else
// (SASOS), but its own page tree cloned from g_as_template: isolation is
// which tree carries PAGE_U leaves, enforced by a per-process CR3.
struct process *process_create_user(uint64_t uid);

// Spawn a user thread that starts at `entry` in ring 3 on `user_stack_top`.
// Caller must have mapped entry/stack PAGE_U beforehand.
struct thread *uthread_spawn(struct process *proc, uint64_t entry,
                             uint64_t user_stack_top);

// As uthread_spawn, but passes `arg` to the entry point in the Win64 first
// argument register (rcx). Lets the kernel hand a freshly spawned process a
// single bootstrap word (e.g. a role selector or a peer pid) with no shared
// memory. uthread_spawn is this with arg == 0.
struct thread *uthread_spawn_arg(struct process *proc, uint64_t entry,
                                 uint64_t user_stack_top, uint64_t arg);

// Currently running thread on this CPU. Only meaningful from contexts
// that cannot migrate (IRQs off / interrupt context).
struct thread *thread_current(void);

// Park the current *user* thread from syscall/interrupt context (IRQ
// depth exactly 1, i.e. the irq_enter of the current entry). The caller
// must already have saved the trap frame via arch_uthread_save_frame
// (except for exit, where the context is dead anyway). Never returns to
// the caller: control goes to the per-CPU scheduler loop, and the thread
// itself, if it runs again, resumes in ring 3 from its saved frame.
[[noreturn]] void uthread_park_exit(void);     // THREAD_DEAD
[[noreturn]] void uthread_park_yield(void);    // requeue on this CPU
[[noreturn]] void uthread_park_blocked(void);  // caller arranged a wake-up

// Deliver `v` as the wake-up payload for a blocked thread: saved-frame rax
// for a parked user thread, wait_result for a kernel thread. Call before
// thread_unblock.
void thread_deliver_wait_result(struct thread *t, uint64_t v);

#endif // thread_h_INCLUDED
