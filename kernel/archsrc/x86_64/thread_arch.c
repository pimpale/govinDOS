#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "gdt.h"
#include "irq.h"
#include "paging.h"
#include "stdlib/string.h"
#include "thread.h"
#include "trap_frame.h"

// Defined in context_switch.asm — first instruction of every freshly
// spawned thread. After switch_context's pop sequence, control returns
// into this label with rdi=entry, rsi=arg, and then it bounces into the
// portable thread_trampoline.
extern void thread_bootstrap(void);

// Forge a stack frame so that the first switch_context into this thread
// lands in thread_bootstrap with the right argument registers loaded.
//
// Layout pushed onto the stack (high addresses first; switch_context's
// pop sequence consumes from low addresses upward):
//
//   [stack_top - 8]    ret address = thread_bootstrap
//   [-16]              rbp = 0
//   [-24]              rbx = 0
//   [-32]              rdi = entry      (Sys-V arg0; bootstrap copies to rcx)
//   [-40]              rsi = arg        (Sys-V arg1; bootstrap copies to rdx)
//   [-48]              r12 = 0
//   [-56]              r13 = 0
//   [-64]              r14 = 0
//   [-72]              r15 = 0
//   [-80]              rflags = 0x002   (IF=0; bit 1 reserved-1)
//
// arch.kernel_rsp points at the rflags slot, i.e. stack_top - 80.
//
// IF is left 0 because the scheduler enters us with IRQs masked (depth=1
// from the top-of-iteration irq_disable in scheduler_loop). The portable
// thread_enter balances that with irq_enable before invoking user code.
void arch_thread_init_kernel(struct thread *t,
                             void (*entry)(void *),
                             void *arg) {
  asserts(((uintptr_t)t->stack_top & 0xF) == 0,
          "arch_thread_init_kernel: stack_top must be 16-aligned");

  uint64_t *sp = (uint64_t *)t->stack_top;
  *--sp = (uint64_t)thread_bootstrap; // return address consumed by `ret`
  *--sp = 0;                          // rbp
  *--sp = 0;                          // rbx
  *--sp = (uint64_t)entry;            // rdi
  *--sp = (uint64_t)arg;              // rsi
  *--sp = 0;                          // r12
  *--sp = 0;                          // r13
  *--sp = 0;                          // r14
  *--sp = 0;                          // r15
  *--sp = 0x002;                      // rflags: IF=0, reserved bit 1 set

  t->arch.kernel_rsp = (uint64_t)sp;
  t->arch.xsave_area = nullptr;
}

// ---------------------------------------------------------------------------
// User threads (stackless in the kernel)
// ---------------------------------------------------------------------------

// Defined in context_switch.asm. Ret target of the forged resume frame.
extern void uthread_resume(void);

// Re-arm the forged switch_context frame at the top of resume_stack and
// point kernel_rsp at it. Layout mirrors arch_thread_init_kernel, with
// uthread_resume as the ret target and rdi = the thread itself. Contents
// are constant per thread, but re-forging on every park keeps the "frame
// is consumed by each resume" invariant obvious and preemption-proof.
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

void arch_thread_init_user(struct thread *t, uint64_t entry,
                           uint64_t user_stack_top) {
  memset(&t->arch.uframe, 0, sizeof(t->arch.uframe));
  t->arch.uframe.rip = entry;
  t->arch.uframe.cs = GDT_SEL_USER_CS64;
  t->arch.uframe.ss = GDT_SEL_USER_DS;
  t->arch.uframe.rsp = user_stack_top;
  // IF=1 (ring 3 always runs with interrupts on), reserved bit 1.
  t->arch.uframe.rflags = 0x202;
  t->arch.xsave_area = nullptr;
  uthread_reset_resume_frame(t);
}

void arch_uthread_save_frame(struct thread *t, const struct trap_frame *tf) {
  t->arch.uframe = *tf;
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
  // Future: restore FSBASE from t->user_tls_base, XRSTOR t->arch.xsave_area.
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
//   - Restore FPU/SSE/AVX state — left lazy until the FPU is actually
//     touched by user code.
void arch_thread_install(struct thread *t) {
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];

  // Avoid a CR3 write when the AS isn't actually changing — that flush
  // is expensive and most kernel-thread switches stay in g_as_kernel.
  if (t->proc != nullptr && t->proc->as != nullptr &&
      t->proc->as != cs->current_as) {
    as_switch(t->proc->as);
  }

  // Load-bearing invariant (memory-design §3): kthread stacks are only
  // ever touched while g_as_kernel is current — their guard pages exist
  // in no other AS, so violating this silently disables overflow
  // detection. We are about to switch onto this thread's stack.
  asserts(t->proc->uid != 0 || cs->current_as == g_as_kernel,
          "kthread dispatched on a non-kernel address space");
}
