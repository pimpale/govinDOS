#ifndef process_h_INCLUDED
#define process_h_INCLUDED

#include <stdint.h>

#include <gdosabi/thread.h>

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
// mechanism for teardown is the syscall boundary itself. Descendant death
// is logically immediate through process_is_dead(), then materialized one
// process at a time during post-order reap.
//
// All tree/state mutations run under g_umem, the umem control-plane
// lock (hierarchy in umem.h) — the same lock that guards share edges
// and ring state — so liveness checks, revocation, and death can never
// disagree.

// Create an embryo. `parent` may be nullptr only for kernel-driven
// creations (init, boot selftests).
struct process *process_create(struct process *parent);

// Spawn a thread into `p` (kernel-driven: init's first thread, boot
// tests), sealing an embryo. Takes the umem lock. Stack bounds and guards are
// userspace policy; start->stack_pointer is only the initial user RSP.
struct thread *process_spawn_thread(struct process *p,
                                    const struct gdos_thread_start *start);

// Effective liveness: true when p or any immutable ancestor is directly
// PROC_DEAD. Safe for lock-free checkpoint reads because process structs are
// reaped descendants-first, so an extant process's ancestor chain is live.
bool process_is_dead(const struct process *p);

// Mark `p` directly dead. Every descendant becomes effectively dead through
// process_is_dead without an eager subtree walk. Post KEV_CHILD_DEAD for the
// subtree root; running/runnable threads are culled at checkpoints/dispatch,
// and blocked TCBs are detached and batch-freed by bounded reap.
void process_kill_subtree(struct process *p);

// One bounded reap step on the dead subtree rooted at `target` (which
// must already be dead): frees one owned block of the deepest unreaped
// zombie, drops one shared-in view, frees up to 256 detached blocked TCBs,
// or (once scheduler-owned threads are culled and its AS has drained off
// every CPU) frees the AS/process. Returns REAP_MORE / REAP_DONE /
// SYSERR_AGAIN (still draining). Takes g_umem.
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
                               uint64_t start_ptr, uint64_t start_size);
[[noreturn]] void proc_sys_process_exit(struct thread *curr, uint64_t status);
uint64_t proc_sys_kill(struct thread *curr, uint64_t pid);
uint64_t proc_sys_reap(struct thread *curr, uint64_t pid);
uint64_t proc_sys_vm_move(struct thread *curr, uint64_t base, uint64_t pid);
uint64_t proc_sys_vm_protect_for(struct thread *curr, uint64_t base,
                                 uint64_t len, uint64_t prot, uint64_t pid);

#endif // process_h_INCLUDED
