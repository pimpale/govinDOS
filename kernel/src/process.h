#ifndef process_h_INCLUDED
#define process_h_INCLUDED

#include <stdint.h>

#include "thread.h"

// Process trees, parent-driven creation, and parent-driven teardown
// (ipc-process-design.md §4, §5).
//
// Parents build children from their own resources: SYS_PROC_CREATE mints
// an embryo (AS clone, no threads), the parent transfers blocks into it
// (SYS_VM_MOVE), sets view protections (SYS_VM_PROTECT with a pid), and
// the first SYS_THREAD_SPAWN seals it. Death cascades down the tree —
// killing a process (or its own exit) kills every descendant — and
// reclamation climbs back up: the dead stay zombies, holding and being
// charged for everything they own, until the parent's SYS_PROC_REAP loop
// takes them apart one bounded step at a time. Nothing ever reparents.
//
// There is no kernel work queue and no reaper thread: the chunking
// mechanism for teardown is the syscall boundary itself.
//
// All tree/state mutations run under the umem lock — the same lock that
// guards blocks, share edges, and channel state — so liveness checks,
// revocation, and death can never disagree.

// Create an embryo. `parent` may be nullptr only for kernel-driven
// creations (init, boot selftests). The child inherits `uid` from the
// caller at the syscall layer; kernel callers pass it explicitly.
struct process *process_create(struct process *parent, uint64_t uid);

// Spawn a thread into `p` (kernel-driven: init's first thread, boot
// tests), sealing an embryo. Takes the umem lock.
struct thread *process_spawn_thread(struct process *p, uint64_t entry,
                                    uint64_t stack_top, uint64_t arg);

// Mark `p` and every descendant dead: remove them from the live-pid
// index, post KEV_CHILD_DEAD for the subtree root (interior parents are
// themselves dying), unhook and wake each victim thread parked in a
// channel waiter slot. Running threads die at their next kernel entry;
// runnable ones are culled at dispatch. O(subtree). Takes the umem lock.
void process_kill_subtree(struct process *p);

// One bounded reap step on the dead subtree rooted at `target` (which
// must already be dead): frees one owned block of the deepest unreaped
// zombie, or drops one of its shared-in views, or (once its threads are
// culled and its AS has drained off every CPU) frees the AS, or frees
// the process struct itself. Returns REAP_MORE / REAP_DONE /
// SYSERR_AGAIN (still draining). Takes the umem lock.
uint64_t process_reap_step(struct process *target);

// Called by the scheduler loop (IRQs off, no scheduler lock held) for a
// thread that exited or was culled at dispatch: frees the TCB and, when
// the last thread of a live process goes, triggers the process's death.
void process_thread_exited(struct thread *t);

// The distinguished root process. Its death is a kernel panic.
void process_set_init(struct process *p);

// Syscall backends (syscall.c).
uint64_t proc_sys_create(struct thread *curr);
uint64_t proc_sys_thread_spawn(struct thread *curr, uint64_t pid,
                               uint64_t entry, uint64_t stack_top,
                               uint64_t arg);
uint64_t proc_sys_kill(struct thread *curr, uint64_t pid);
uint64_t proc_sys_reap(struct thread *curr, uint64_t pid);
uint64_t proc_sys_vm_move(struct thread *curr, uint64_t base, uint64_t pid);
uint64_t proc_sys_vm_protect_for(struct thread *curr, uint64_t base,
                                 uint64_t len, uint64_t prot, uint64_t pid);

#endif // process_h_INCLUDED
