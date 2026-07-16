#ifndef gdos_syscall_h_INCLUDED
#define gdos_syscall_h_INCLUDED

#include <stdint.h>

// The govindos syscall ABI — the kernel↔userspace contract, shared
// verbatim by both builds (docs/technical/source-tree.md). One producer,
// one consumer, same repo, same commit.
//
// Register convention (SYSCALL/SYSRET): rax = syscall number, args in
// r10, rdx, r8, r9 (max 4), result in rax; rcx and r11 are clobbered
// (SYSCALL overwrites them with the return rip/rflags — architectural,
// not a choice). Everything else is preserved. r10 stands in for the
// Win64 rcx argument slot exactly as in the Linux convention.
//
// Design rule: kernel work is bounded and non-blocking. The deliberate
// exceptions (SYS_BLOCK_WAIT, SYS_YIELD) park the calling user thread —
// anything long-running is a registration + event on a channel
// (ipc-process-design.md §2), and anything long-lived in teardown is the
// parent's reap loop (§4).

#define SYS_DEBUG_WRITE 0 // (ptr, len)             -> bytes written
#define SYS_EXIT        1 // ()                     -> never returns
#define SYS_YIELD       2 // ()                     -> 0
#define SYS_RESERVED_3  3 // retired GETUID slot; kept reserved for ABI stability
#define SYS_GETPID      4 // ()                     -> pid

// Memory blocks are the vm_alloc unit (power-of-two pages). alloc/free,
// not map/unmap: in a SASOS the whole address space is already mapped in
// principle — these verbs create and destroy blocks (and the views that
// make them accessible), they don't position anything. vm_protect
// re-flags a page-aligned sub-range of a block in the caller's own view
// (or, with a pid, an own embryo's view — the parent applying W^X to the
// image it wrote); prot 0 makes it inaccessible. vm_share maps a whole
// owned block into another process (positive target) or turns it into a
// kernel channel (negative scheme id); the owner freeing the block (or
// dying + being reaped) revokes every view. vm_move transfers ownership
// along tree edges only: down into an own embryo, up out of an own
// zombie child.
#define SYS_VM_ALLOC   5 // (len, prot)             -> base
#define SYS_VM_FREE    6 // (base)                  -> 0
#define SYS_VM_PROTECT 7 // (base, len, prot[, pid])-> 0
#define SYS_VM_SHARE   8 // (base, target, prot)    -> 0 (target signed)
#define SYS_VM_UNSHARE 9 // (base)                  -> 0
#define SYS_VM_MOVE   10 // (base, pid)             -> 0

// Block waits and doorbells (ipc-process-design.md §1). An owned unshared
// block is process-private; a block with one sharer is a two-party channel.
// The block's base is its doorbell name; WAIT takes a word within it.
#define SYS_BLOCK_DOORBELL 11 // (base)           -> 0 (never blocks)
#define SYS_BLOCK_WAIT     12 // (addr, expected) -> 0 (may park; SYSERR_DEAD on revoke)

// Process trees (ipc-process-design.md §5): parent-driven creation
// (embryo -> VM_MOVE/VM_PROTECT -> first THREAD_SPAWN seals), recursive
// kill, and the parent-driven reap loop that replaces all deferred
// kernel teardown.
#define SYS_PROC_CREATE  13 // ()                        -> pid (embryo)
#define SYS_THREAD_SPAWN 14 // (pid, entry, stack, arg)  -> tid (pid: self or own embryo)
#define SYS_PROC_KILL    15 // (pid)                     -> 0 (own descendant; subtree dies)
#define SYS_PROC_REAP    16 // (pid)                     -> REAP_* | SYSERR_AGAIN (own dead child)

// Map the exact range named by a live GRANT_DEVMEM token as a device-backed
// ublock. flags must be a subset of the grant flags. Success returns 0.
#define SYS_VM_MAP_DEVICE 17 // (token_ptr, token_len, flags) -> 0
#define SYS_VM_SIZE       18 // (base)                    -> block bytes

#define SYS_MAX          19

// SYS_PROC_REAP results: one more bounded step done / the subtree is
// fully gone. SYSERR_AGAIN means culling/drain hasn't caught up — call
// again.
#define REAP_DONE 0
#define REAP_MORE 1

// vm_alloc prot bits (user-facing; translated to paging flags internally).
#define VM_PROT_READ  1u
#define VM_PROT_WRITE 2u
#define VM_PROT_EXEC  4u

// SYS_VM_MAP_DEVICE flags. Ordinary device memory is UC. FIRMWARE selects
// the read-only WB exception for ACPI backing pages; WC is reserved until a
// platform allowlist exists.
#define VM_DEVICE_READ     1u
#define VM_DEVICE_WRITE    2u
#define VM_DEVICE_WC       4u
#define VM_DEVICE_FIRMWARE 8u

// Errors are returned as small negative values in rax.
#define SYSERR_NOSYS ((uint64_t) - 1)
#define SYSERR_FAULT ((uint64_t) - 2)
#define SYSERR_INVAL ((uint64_t) - 3)
#define SYSERR_NOMEM ((uint64_t) - 4)
#define SYSERR_EXIST ((uint64_t) - 5)
#define SYSERR_PERM  ((uint64_t) - 6)
#define SYSERR_DEAD  ((uint64_t) - 7) // channel peer revoked/died (wakes a parked waiter)
#define SYSERR_AGAIN ((uint64_t) - 8) // operation incomplete or no slot; retry

#endif // gdos_syscall_h_INCLUDED
