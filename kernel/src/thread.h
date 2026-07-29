#ifndef thread_h_INCLUDED
#define thread_h_INCLUDED

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// archsrc/<arch>/arch_thread.h provides struct arch_thread, the arch-private
// per-thread state (saved kernel SP, FPU/SIMD save area, etc.).
#include "arch_thread.h"
#include "spinlock.h"

struct address_space;
struct grant;
typedef struct process process;

// There are no kernel threads (ipc-process-design.md §4): the only
// per-CPU kernel contexts are the scheduler/idle loops on the bootstrap
// stacks, and every thread is a user thread. A user thread is stackless
// in the kernel — its suspended state is the trap frame saved in the TCB
// by arch_uthread_save_frame — and it is resumed through a forged frame
// that irets straight back to ring 3. A thread may therefore only park
// at a point where its entire kernel state is that frame, i.e. from the
// syscall/interrupt path after the frame has been saved. That is what
// keeps the single-kernel-stack-per-CPU model sound.

enum thread_status {
  THREAD_RUNNABLE,
  THREAD_RUNNING,
  THREAD_BLOCKED,
  THREAD_DEAD,
};

// Futex wait arbitration (futex-design.md §6). Every party that wants a
// parked thread — waker, requeue, deadline expiry, reap — goes through
// wake_state:
//   PARKED -> CLAIMED   the winner's claim; a reap-visible lifetime pin.
//                       The winner removes both tree entries, then either
//                       releases through thread_unblock_claimed (live
//                       process) or publishes REAPABLE (dead process).
//   PARKED -> MOVING    requeue's relink window: strictly pointer work,
//                       spun on briefly by expiry, skipped by reap.
//   REAPABLE            terminal; reap frees the TCB directly (the winner
//                       already removed every tree entry).
enum futex_state : uint32_t {
  FUTEX_IDLE,
  FUTEX_PARKED,
  FUTEX_MOVING,
  FUTEX_CLAIMED,
  FUTEX_REAPABLE,
};

// Bucket-tree key: waiters on one address are contiguous and in FIFO
// order via the global park sequence (futex.c).
struct futex_key {
  uint64_t address;
  uint64_t seq;
};

typedef struct thread {
  uint64_t tid;
  struct process *proc; // never null
  _Atomic enum thread_status status;

  // True from just before the scheduler switches into this thread until
  // just after it has switched back out and its saved state is complete.
  // Wakers must spin this false before re-enqueueing
  // (thread_unblock_claimed does) — otherwise a thread that has set
  // status=BLOCKED but not yet finished saving its context could be
  // dispatched on another CPU.
  _Atomic bool on_cpu;

  // Futex wait state (futex.c). wait_key is written under the bucket at
  // park (and rewritten under both buckets by requeue's MOVING window);
  // it is how wakers, expiry, and reap find the tree node to remove.
  // deadline/deadline_cpu name the armed entry in a per-CPU deadline
  // tree (timer.c); deadline 0 means not armed. wake_state is the claim
  // arbitration above.
  struct futex_key wait_key;
  uint64_t deadline;
  uint32_t deadline_cpu;
  _Atomic enum futex_state wake_state;

  // Optional join completion. A live-process exit consumes one pin and
  // publishes the word after on_cpu becomes false. Process death skips the
  // notification because no in-process joiner can survive it.
  struct ublock *completion_block;
  uint64_t completion_event;

  // Arch-private state (saved kernel SP, trap frame, FPU area, ...).
  struct arch_thread arch;
} thread;

// Element type for generic containers that hold non-owning thread references.
// The vec/list templates require a single-identifier type name for token
// pasting, hence the typedef rather than using `thread *` directly.
typedef thread *thread_ptr;

#define LLRB_NAME tid_thread
#define LLRB_KEY uint64_t
#define LLRB_VALUE thread_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_tid_thread_node
#define SLAB_TYPE llrb_tid_thread_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

struct llrb_pid_process;
struct llrb_base_block;
struct llrb_base_edge;

struct process {
  // Never reused: pids are minted monotonically (2^63 outlast the
  // hardware), so a (pid, base) pair is forever unambiguous and there is
  // no ABA on cross-process references.
  uint64_t pid;
  // Every process gets its own as_clone(g_as_kernel). All ASes share the
  // identity layout (SASOS) — isolation is which tree carries PAGE_U
  // leaves for a block.
  struct address_space *as;
  // Direct death mark, guarded for writes by g_umem. A process starts alive
  // even though it has no threads; its direct parent may configure it and
  // spawn threads throughout its lifetime. Descendants of a dead ancestor
  // are effectively dead without changing this bit; process_is_dead() is
  // the liveness test.
  _Atomic bool dead;

  // The creation edge. Death follows children downward, destruction is
  // post-order upward, and the tree is never restructured.
  struct process *parent; // nullptr for init and boot-test processes
  struct llrb_pid_process *children;

  // KEV_CHILD_DEAD has been posted to the parent's tree channel (or the
  // parent was dying and never gets one). Clear means the event is still
  // pending in this level state, replayed when the parent's tree channel
  // exists and has room.
  bool death_notified;

  // Process-wide exit status reported in KEV_CHILD_DEAD.b. Natural last-
  // thread exit and external kill leave the zero-initialized value.
  uint64_t exit_status;

  // Threads with undisposed TCBs. Incremented at spawn; decremented by the
  // scheduler's exit/cull path or exact destruction of blocked TCBs.
  // The process struct outlives every TCB.
  _Atomic uint64_t nthreads;
  llrb_tid_thread *threads;

  // User memory (umem.c): blocks this process owns, and share edges for
  // blocks other processes have shared with it. Both ordered indices are
  // guarded by ulock for ALL access, read or write.
  struct svclock ulock;
  struct llrb_base_block *blocks;
  struct llrb_base_edge *views;

  // Intrusive queue of share edges whose KEV_SHARE level event has not yet
  // been posted. Membership is exactly `!edge->notified`.
  struct share_edge *unnotified_head;
  struct share_edge *unnotified_tail;

  // Kernel scheme endpoints (channel.c, umem lock): the shares channel
  // (-1) and the tree channel (-3), each at most one per process,
  // pointing at rings hanging off owned blocks; cleared by the revoke
  // path when those blocks die.
  struct ring *share_ch;
  struct ring *tree_ch;
  struct ring *iommu_ch;

};

#define SLAB_NAME thread
#define SLAB_TYPE thread
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME process
#define SLAB_TYPE process
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

// Spawn a user thread that starts at `entry` in ring 3 with `user_stack_pointer`
// with `arg` in its first argument register (rcx under the Win64-flavored
// ABI, so `entry` can be a plain `void f(uint64_t)`). Caller must have
// mapped entry/stack PAGE_U beforehand.
struct thread *uthread_spawn(struct process *proc, uint64_t entry,
                             uint64_t user_stack_pointer, uint64_t arg,
                             uint64_t fs_base, uint64_t gs_base,
                             struct ublock *completion_block,
                             uint64_t completion_event);

// Currently running thread on this CPU. Only meaningful from contexts
// that cannot migrate (IRQs off / interrupt context).
struct thread *thread_current(void);

// Park the current user thread from syscall/interrupt context (IRQ
// depth exactly 1, i.e. the irq_enter of the current entry). The caller
// must already have saved the trap frame via arch_uthread_save_frame
// (except for exit, where the context is dead anyway). Never returns to
// the caller: control goes to the per-CPU scheduler loop, and the thread
// itself, if it runs again, resumes in ring 3 from its saved frame.
[[noreturn]] void uthread_park_exit(void);    // THREAD_DEAD
[[noreturn]] void uthread_park_yield(void);   // requeue on this CPU
[[noreturn]] void uthread_park_blocked(void); // caller arranged a wake-up

// Release a thread the caller has CLAIMED (futex-design.md §6): spin out
// the in-flight context save, flip BLOCKED -> RUNNABLE, release the claim
// (CLAIMED -> IDLE), then enqueue. After the enqueue the caller may not
// touch the TCB at all — the thread can run, exit, and be destroyed.
void thread_unblock_claimed(struct thread *t);

// Deliver `v` as the wake-up payload for a blocked thread: it lands in
// the saved frame's rax and becomes the syscall return value when the
// thread irets back to ring 3. Call before thread_unblock_claimed.
void thread_deliver_wait_result(struct thread *t, uint64_t v);

#endif // thread_h_INCLUDED
