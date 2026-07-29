#ifndef process_h_INCLUDED
#define process_h_INCLUDED

#include <stdint.h>

#include <gdosabi/thread.h>

#include "thread.h"

// Process trees, parent-driven creation, and parent-driven teardown
// (ipc-process-design.md §4, §5).
//
// Parents build and manage children from their own resources:
// SYS_PROC_CREATE mints an alive process with an AS and no threads; its
// direct parent may transfer blocks into it, set view protections, and
// spawn threads throughout its lifetime. Death cascades down the tree —
// killing a process (or its own exit) kills every descendant — and
// reclamation is explicit: the dead stay zombies until an authorized
// ancestor enumerates and destroys their resources and finally calls
// SYS_PROC_DESTROY on the empty process body. Nothing ever reparents.
//
// There is no kernel teardown queue or teardown thread. Descendant death is
// logically immediate through process_is_dead(); userspace enumerates and
// removes resources before exact post-order PROC_DESTROY.
//
// All tree/state mutations run under g_umem, the umem control-plane
// lock (hierarchy in umem.h) — the same lock that guards share edges
// and ring state — so liveness checks, revocation, and death can never
// disagree.

// Create an alive zero-thread process. `parent` may be nullptr only for
// kernel-driven creations (init, boot selftests).
struct process *process_create(struct process *parent);

// Spawn a thread into `p` (kernel-driven: init's first thread, boot tests).
// Takes the umem lock. Stack bounds and guards are userspace policy;
// start->stack_pointer is only the initial user RSP.
struct thread *process_spawn_thread(struct process *p,
                                    const struct gdos_thread_start *start);

// Effective liveness: true when p or any immutable ancestor has its direct
// death bit set. Safe for lock-free checkpoint reads because process structs
// are destroyed descendants-first, so an extant process's ancestor chain is
// live.
bool process_is_dead(const struct process *p);

// Mark `p` directly dead. Every descendant becomes effectively dead through
// process_is_dead without an eager subtree walk. Post KEV_CHILD_DEAD for the
// subtree root; running/runnable threads are culled at checkpoints/dispatch,
// and blocked TCBs are detached by exact SYS_THREAD_DESTROY calls.
void process_kill_subtree(struct process *p);

// True for an exact target reachable through an all-dead strict-descendant
// path from caller. Caller holds g_umem.
bool process_reaper_authorized_locked(const struct process *caller,
                                      const struct process *target);

// Called by the scheduler loop (IRQs off, no scheduler lock held) for a
// thread that exited or was culled at dispatch: frees the TCB and, when
// the last thread of a live process goes, triggers the process's death.
void process_thread_exited(struct thread *t);

// The distinguished root process. Its death is a kernel panic.
void process_set_init(struct process *p);

// Syscall backends (syscall.c).
uint64_t proc_sys_create(struct thread *curr);
uint64_t proc_sys_thread_spawn(struct thread *curr, uint64_t pid,
                               uint64_t start_ptr, uint64_t start_size);
[[noreturn]] void proc_sys_process_exit(struct thread *curr, uint64_t status);
uint64_t proc_sys_kill(struct thread *curr, uint64_t pid);
uint64_t proc_sys_threads(struct thread *curr, uint64_t pid, uint64_t buf,
                          uint64_t cap, uint64_t after);
uint64_t proc_sys_thread_destroy(struct thread *curr, uint64_t pid,
                                 uint64_t tid);
uint64_t proc_sys_children(struct thread *curr, uint64_t pid, uint64_t buf,
                           uint64_t cap, uint64_t after);
uint64_t proc_sys_destroy(struct thread *curr, uint64_t pid);
uint64_t proc_sys_vm_move(struct thread *curr, uint64_t base, uint64_t pid);
uint64_t proc_sys_vm_protect_for(struct thread *curr, uint64_t base,
                                 uint64_t len, uint64_t prot, uint64_t pid);

#endif // process_h_INCLUDED
