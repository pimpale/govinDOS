#ifndef trap_frame_h_INCLUDED
#define trap_frame_h_INCLUDED

#include <stdint.h>

struct thread;

// Full register snapshot of an interrupted context, as laid out on the
// stack by the ISR stubs in interrupts.asm. Field order must stay in sync
// with the push sequence there (stub pushes rdx/r8/r9, _isr_handler pushes
// the rest; the CPU pushes the error code and the iretq frame).
//
// This one layout serves three roles:
//   - the register view handed to interrupt_handler / syscall_entry
//   - the syscall argument/return convention (rax = nr/result; args in
//     rcx, rdx, r8, r9 — plain Win64, viable because `int 0x80` clobbers
//     nothing, unlike SYSCALL)
//   - the per-thread saved user context (arch_thread.uframe) that a parked
//     user thread is resumed from. In the single-kernel-stack-per-CPU
//     model, a user thread may only block when its entire kernel state is
//     this frame; parking copies it off the per-CPU stack into the TCB and
//     uthread_resume irets from the TCB copy.
struct [[gnu::packed]] trap_frame {
  // pushed by _isr_handler (rax lowest)
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  // pushed by the per-vector stub
  uint64_t r9;
  uint64_t r8;
  uint64_t rdx;
  // pushed by the CPU (or a fake 0 from the stub)
  uint64_t error;
  // hardware iretq frame; rsp/ss are always pushed in 64-bit mode
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
};

static_assert(sizeof(struct trap_frame) == 21 * 8,
              "trap_frame layout must match interrupts.asm");

// ---------------------------------------------------------------------------
// Arch hooks for user threads (implemented in thread_arch.c /
// context_switch.asm). Portable code calls these through syscall/thread
// paths; `struct thread` stays opaque here.
// ---------------------------------------------------------------------------

// Initialize a fresh user thread: zeroed GPRs except `arg` in the first
// argument register, rip/rsp at the given user entry/stack, user CS/SS,
// IF set. Also forges the TCB-resident resume frame so the scheduler's
// switch_context lands in uthread_resume.
void arch_thread_init_user(struct thread *t, uint64_t entry,
                           uint64_t user_stack_pointer, uint64_t arg,
                           uint64_t fs_base, uint64_t gs_base);

// Copy the live trap frame (on the per-CPU interrupt stack) into the TCB
// and re-arm the resume frame. Must be called before parking a user
// thread; after this the per-CPU stack contents are dead.
void arch_uthread_save_frame(struct thread *t, const struct trap_frame *tf);

// Set the syscall return value a parked user thread will see when it
// resumes (its saved rax). Used by wakers that complete a blocked syscall.
void arch_uthread_set_result(struct thread *t, uint64_t v);

#endif // trap_frame_h_INCLUDED
