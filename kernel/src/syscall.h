#ifndef syscall_h_INCLUDED
#define syscall_h_INCLUDED

#include <stdint.h>

// System call ABI (int 0x80 era).
//
// User side is plain Win64: rax = syscall number, args in rcx, rdx, r8,
// r9 (max 4), result in rax. `int 0x80` clobbers nothing, so a user stub
// is literally `mov eax, N ; int 0x80 ; ret` and behaves like any Win64
// call (kernel restores every register except rax).
//
// Kernel-internal convention: arguments live in the trap frame's
// rcx/rdx/r8/r9 slots. A future SYSCALL/SYSRET entry stub shuffles
// r10 -> frame.rcx (SYSCALL clobbers rcx) and the dispatcher never
// notices the difference.
//
// Design rule: short syscalls must not block. The deliberate exceptions
// (SYS_DUMMY_READ, SYS_RING_WAIT, SYS_YIELD) park the calling user thread
// via the TCB-frame save — anything long-running belongs on the ring.

#define SYS_DEBUG_WRITE 0 // (ptr, len)             -> bytes written
#define SYS_EXIT        1 // ()                     -> never returns
#define SYS_YIELD       2 // ()                     -> 0
#define SYS_GETUID      3 // ()                     -> uid
#define SYS_GETPID      4 // ()                     -> pid
#define SYS_VM_MAP      5 // (len, prot)            -> base
#define SYS_VM_UNMAP    6 // (base, len)            -> 0
#define SYS_DUMMY_READ  7 // ()                     -> value (blocks)
#define SYS_RING_CREATE 8 // ()                     -> ring base
#define SYS_RING_ENTER  9 // ()                     -> 0 (doorbell)
#define SYS_RING_WAIT  10 // (my_cq_head)           -> 0 (blocks if empty)

// Memory blocks are the vm_map unit (power-of-two pages). vm_protect
// re-flags a page-aligned sub-range of a block in the caller's own view;
// prot 0 makes it inaccessible. vm_share maps a whole owned block into
// another process; the owner freeing the block (or dying) revokes it.
#define SYS_VM_PROTECT 11 // (base, len, prot)      -> 0
#define SYS_VM_SHARE   12 // (base, pid, prot)      -> 0
#define SYS_VM_UNSHARE 13 // (base)                 -> 0

#define SYS_MAX        14

// vm_map prot bits (user-facing; translated to paging flags internally).
#define VM_PROT_READ  1u
#define VM_PROT_WRITE 2u
#define VM_PROT_EXEC  4u

// Errors are returned as small negative values in rax.
#define SYSERR_NOSYS ((uint64_t) - 1)
#define SYSERR_FAULT ((uint64_t) - 2)
#define SYSERR_INVAL ((uint64_t) - 3)
#define SYSERR_NOMEM ((uint64_t) - 4)
#define SYSERR_EXIST ((uint64_t) - 5)
#define SYSERR_PERM  ((uint64_t) - 6)

struct trap_frame;

// Entry from the arch interrupt path for vector 0x80. Runs at IRQ depth 1
// on the per-CPU interrupt stack. Writes the result into tf->rax and
// returns — except for ops that park the calling thread, in which case it
// never returns and control resumes in the scheduler loop.
void syscall_entry(struct trap_frame *tf);

#endif // syscall_h_INCLUDED
