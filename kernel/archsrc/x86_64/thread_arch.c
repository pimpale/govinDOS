#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "gdt.h"
#include "irq.h"
#include "paging.h"
#include "stdlib/string.h"
#include "thread.h"
#include "trap_frame.h"

// ---------------------------------------------------------------------------
// User threads (stackless in the kernel — the only kind there is)
// ---------------------------------------------------------------------------

// Defined in context_switch.asm. Ret target of the forged resume frame.
extern void uthread_resume(void);

// Re-arm the forged switch_context frame at the top of resume_stack and
// point kernel_rsp at it: the callee-saved pop sequence of
// switch_context, with uthread_resume as the ret target and rdi = the
// thread itself. Contents are constant per thread, but re-forging on
// every park keeps the "frame is consumed by each resume" invariant
// obvious and preemption-proof.
static void uthread_reset_resume_frame(struct thread *t) {
  uint64_t *sp =
      (uint64_t *)(t->arch.resume_stack + sizeof(t->arch.resume_stack));
  *--sp = (uint64_t)uthread_resume;   // ret target
  *--sp = 0;                          // rbp
  *--sp = 0;                          // rbx
  *--sp = (uint64_t)t;                // rdi
  *--sp = 0;                          // rsi
  *--sp = 0;                          // r12
  *--sp = 0;                          // r13
  *--sp = 0;                          // r14
  *--sp = 0;                          // r15
  *--sp = 0x002;                      // rflags: IF=0 until iretq
  t->arch.kernel_rsp = (uint64_t)sp;
}

// Architectural reset state for a fresh thread's FPU: FCW = 0x037F (all
// x87 exceptions masked, 64-bit precision), MXCSR = 0x1F80 (all SSE
// exceptions masked), FTW = 0 (fxsave's abbreviated tag encoding: all
// x87 registers empty), everything else zero. The first resume fxrstors
// this instead of inheriting whatever the previously-running thread left
// in the hardware.
static void fxsave_area_init(uint8_t *area) {
  memset(area, 0, 512);
  *(uint16_t *)(area + 0) = 0x037F;  // FCW
  *(uint32_t *)(area + 24) = 0x1F80; // MXCSR
}

void arch_thread_init_user(struct thread *t, uint64_t entry,
                           uint64_t user_stack_top, uint64_t arg) {
  memset(&t->arch.uframe, 0, sizeof(t->arch.uframe));
  t->arch.uframe.rip = entry;
  // First argument register of the Win64-flavored user ABI: the entry
  // point sees `arg` as a plain function parameter.
  t->arch.uframe.rcx = arg;
  t->arch.uframe.cs = GDT_SEL_USER_CS64;
  t->arch.uframe.ss = GDT_SEL_USER_DS;
  t->arch.uframe.rsp = user_stack_top;
  // IF=1 (ring 3 always runs with interrupts on), reserved bit 1.
  t->arch.uframe.rflags = 0x202;
  fxsave_area_init(t->arch.fxsave_area);
  uthread_reset_resume_frame(t);
}

void arch_uthread_save_frame(struct thread *t, const struct trap_frame *tf) {
  t->arch.uframe = *tf;
  // The GPRs above plus the FPU/SSE registers are the thread's complete
  // user state (the kernel, built -mgeneral-regs-only, cannot touch the
  // latter, so saving here and restoring at resume is sufficient). With
  // preemption this frame save can land mid-arbitrary-instruction-stream,
  // so no register is dead by ABI convention — save them all.
  __asm__ volatile("fxsave64 %0" : "=m"(t->arch.fxsave_area));
  uthread_reset_resume_frame(t);
}

void arch_uthread_set_result(struct thread *t, uint64_t v) {
  t->arch.uframe.rax = v;
}

// Called by uthread_resume (asm) on the thread's resume_stack, IRQs off.
// The scheduler switched into us holding one irq_disable level; iretq will
// set IF from the saved user rflags, so drop the depth without sti.
struct trap_frame *uthread_resume_prepare(struct thread *t) {
  irq_exit();
  __asm__ volatile("fxrstor64 %0" : : "m"(t->arch.fxsave_area));
  // Future: restore FSBASE from t->user_tls_base.
  return &t->arch.uframe;
}

// Per-cpu setup for switching into `t`. Runs with the scheduler spinlock
// held and interrupts disabled.
//
// What we do:
//   - Switch address spaces if this thread's process uses a different one.
//
// What we don't do:
//   - Touch TSS.rsp0. RSP0 is per-CPU now (set once in cpu_install_gdt_tss),
//     so ring transitions always land on the same per-CPU stack regardless
//     of which thread is current.
//   - Update FSBASE for the user TLS pointer (only needed once userspace
//     threads exist; t->user_tls_base will drive it).
//   - Restore FPU/SSE state — uthread_resume_prepare fxrstors it on the
//     way back into ring 3, which is the only exit from here.
void arch_thread_install(struct thread *t) {
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];

  // Avoid a CR3 write when the AS isn't actually changing — that flush
  // is expensive and consecutive threads of one process share an AS.
  if (t->proc->as != cs->current_as) {
    as_switch(t->proc->as);
  }
}
