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
// exceptions (SYS_FUTEX_WAIT, SYS_YIELD) park the calling user thread —
// anything long-running is a registration + event on a channel
// (ipc-process-design.md §2), and anything long-lived in teardown is the
// parent's explicit destruction choreography (§4).

// Numbers are grouped by ability: identity/time, then memory, then
// sharing, then futexes, then process/thread lifecycle.

#define SYS_DEBUG_WRITE 0 // (ptr, len)              -> bytes written
#define SYS_YIELD       1 // ()                      -> 0
#define SYS_GETPID      2 // ()                      -> pid
#define SYS_GETTID      3 // ()                      -> current tid
// Monotonic nanoseconds since an unspecified boot epoch (timer-design.md).
// The sole clock interface; SYS_FUTEX_WAIT deadlines live in this domain.
#define SYS_GETTIME     4 // ()                      -> monotonic ns

// Memory blocks are the vm_alloc unit (power-of-two pages). alloc/free,
// not map/unmap: in a SASOS the whole address space is already mapped in
// principle — these verbs create and destroy blocks (and the views that
// make them accessible), they don't position anything. vm_protect
// re-flags a page-aligned sub-range of a block in the caller's own view
// (or, with a pid, a direct child's view); prot 0 makes it inaccessible.
// vm_share maps a whole
// owned block into another process (positive target) or turns it into a
// kernel channel (negative scheme id). Free refuses while views remain;
// vm_dropshare is the viewer's own removal and vm_unshare is the owner's
// exact per-edge revocation. vm_move transfers ownership down into a direct
// child; that authority remains after the child starts running.
//
// vm_free is a single bounded transaction: it fails SYSERR_EXIST while
// anything is still attached (sharers, DMA mappings, thread pins). Parked
// futex waiters are NOT an attachment — the kernel never wakes waiters
// on revocation (futex-design.md §5); teardown choreography is
// userspace's (close sentinel -> wake -> peers dropshare -> free).
#define SYS_VM_ALLOC   5 // (len, prot)             -> base
#define SYS_VM_FREE    6 // (base)                  -> 0 | SYSERR_EXIST
#define SYS_VM_PROTECT 7 // (base, len, prot[, pid])-> 0
#define SYS_VM_SIZE    8 // (base)                  -> block bytes

// Sharing and ownership transfer. VM_UNSHARE is the owner's per-edge
// revocation of one sharer's view — the coercion path of orderly
// teardown: a peer that never acks the close sentinel with VM_DROPSHARE
// is revoked, and its later touch of the block is an ordinary
// revocation death. VM_MAP_DEVICE maps the exact range named by a live
// GRANT_DEVMEM token as a device-backed ublock (flags must be a subset
// of the grant flags).
#define SYS_VM_SHARE      9  // (base, target, prot)    -> 0 (target signed)
#define SYS_VM_DROPSHARE  10 // (base, pid)             -> 0 (pid 0 = self)
#define SYS_VM_UNSHARE    11 // (base, pid)             -> 0 (owner revokes one edge)
#define SYS_VM_MOVE       12 // (base, pid)             -> 0
#define SYS_VM_MAP_DEVICE 13 // (token_ptr, token_len, flags) -> 0
#define SYS_VM_SHARERS    14 // (base, buf, cap, after) -> count
#define SYS_VM_BLOCKS     15 // (pid, buf, cap, after)  -> count
#define SYS_VM_VIEWS      16 // (pid, buf, cap, after)  -> count
#define SYS_VM_DMA_MAPS   17 // (base, buf, cap, after) -> count
#define SYS_VM_DMA_REVOKE 18 // (base, domain_id)       -> 0

#define VM_ENUM_BATCH 128u

// Address-keyed waiting (futex-design.md). WAKE resolves addr to a block
// the caller has a view of; if that block is a kernel channel this is the
// doorbell (drain + replay, count ignored), otherwise it wakes up to
// min(count, FUTEX_WAKE_BATCH) threads parked on exactly addr, FIFO, and
// returns how many. WAIT parks while the 32-bit word at addr equals
// expected; wake_addr != 0 fuses a one-shot FUTEX_WAKE(wake_addr, 1)
// before the compare (its failure is ignored); deadline is an absolute
// SYS_GETTIME-domain nanosecond value, 0 = none. REQUEUE moves up to
// min(count, FUTEX_REQUEUE_BATCH) waiters from `from` to `to` in FIFO
// order if the word at `from` equals expected, and returns how many.
#define SYS_FUTEX_WAKE    19 // (addr, count)                      -> nwoken
#define SYS_FUTEX_WAIT    20 // (addr, expected, wake_addr, deadline) -> 0 | SYSERR_*
#define SYS_FUTEX_REQUEUE 21 // (from, to, expected, count)        -> nmoved | SYSERR_*

// Maximum waiters detached by one FUTEX_WAKE / moved by one FUTEX_REQUEUE
// call. Kernel work and runqueue publication are therefore bounded; the
// caller loops while the return equals min(count, batch).
#define FUTEX_WAKE_BATCH 16u
#define FUTEX_REQUEUE_BATCH 16u

// Process trees (ipc-process-design.md §5): parent-driven creation and live
// child management, recursive kill, enumeration-driven resource teardown,
// and exact process destruction.
#define SYS_PROC_CREATE       22 // ()                -> pid (alive, no threads)
#define SYS_THREAD_SPAWN      23 // (pid, start_ptr, start_size) -> tid
#define SYS_THREAD_BASES_SET  24 // (fsbase, gsbase)           -> 0
#define SYS_THREAD_EXIT       25 // ()                         -> never returns
#define SYS_PROC_KILL         26 // (pid)                      -> 0
#define SYS_THREADS           27 // (pid, buf, cap, after)     -> count
#define SYS_THREAD_DESTROY    28 // (pid, tid)                 -> 0
#define SYS_PROC_CHILDREN     29 // (pid, buf, cap, after)     -> count
#define SYS_PROC_DESTROY      30 // (pid) -> PROC_DESTROY_* | SYSERR_*
#define SYS_PROC_EXIT         31 // (status)                   -> never returns

#define SYS_MAX               32

#define PROC_DESTROY_DONE 0
#define PROC_DESTROY_MORE 1

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
#define SYSERR_DEAD  ((uint64_t) - 7) // reserved (no current path returns it)
#define SYSERR_AGAIN ((uint64_t) - 8) // FUTEX_WAIT/REQUEUE compare mismatch; retry
#define SYSERR_TIMEDOUT ((uint64_t) - 9) // the deadline passed while parked

#endif // gdos_syscall_h_INCLUDED
