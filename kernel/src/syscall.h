#ifndef syscall_h_INCLUDED
#define syscall_h_INCLUDED

// The syscall numbers, prot bits, SYSERR values, and the register
// convention all live in the shared ABI header (abi/gdos/ — the
// kernel↔userspace contract). This header adds only the kernel-side
// dispatcher.
//
// Kernel-side note on the convention: the entry stub (interrupts.asm)
// stores the user's r10 into the trap frame's rcx slot, so from here on
// the convention is plain Win64 — arguments live in the frame's
// rcx/rdx/r8/r9.

#include <gdos/syscall.h>

struct trap_frame;

// Dispatcher. Runs at IRQ depth 1 on the per-CPU RSP0 stack. Writes the
// result into tf->rax and returns — except for ops that park the calling
// thread, in which case it never returns and control resumes in the
// scheduler loop.
void syscall_entry(struct trap_frame *tf);

// Entry from the SYSCALL stub (interrupts.asm), ring 3 only: brackets
// syscall_entry in the irq_enter/irq_exit the interrupt path gets from
// interrupt_handler (IA32_FMASK cleared IF before we got here).
void syscall_entry_from_user(struct trap_frame *tf);

#endif // syscall_h_INCLUDED
