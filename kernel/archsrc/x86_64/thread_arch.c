#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "gdt.h"
#include "paging.h"
#include "thread.h"

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
//   [kernel_stack_top - 8]   ret address = thread_bootstrap
//   [-16]                    rbp = 0
//   [-24]                    rbx = 0
//   [-32]                    rdi = entry      (Sys-V arg0; bootstrap copies to rcx)
//   [-40]                    rsi = arg        (Sys-V arg1; bootstrap copies to rdx)
//   [-48]                    r12 = 0
//   [-56]                    r13 = 0
//   [-64]                    r14 = 0
//   [-72]                    r15 = 0
//   [-80]                    rflags = 0x002   (IF=0; bit 1 reserved-1)
//
// arch.kernel_rsp points at the rflags slot, i.e. kernel_stack_top - 80.
//
// IF is left 0 because we enter holding the scheduler spinlock; the portable
// thread_trampoline releases the lock and sti's before invoking user code.
void arch_thread_init_kernel(struct thread *t,
                             void (*entry)(void *),
                             void *arg) {
  asserts(((uintptr_t)t->kernel_stack_top & 0xF) == 0,
          "arch_thread_init_kernel: kernel_stack_top must be 16-aligned");

  uint64_t *sp = (uint64_t *)t->kernel_stack_top;
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

// Per-cpu setup for switching into `t`. Runs with the scheduler spinlock
// held and interrupts disabled.
//
// What we do:
//   - Update TSS.rsp0 so the next ring-3 -> ring-0 transition lands on
//     this thread's kernel stack rather than a stale one.
//   - Switch address spaces if this thread's process uses a different one.
//
// What we don't (yet) do:
//   - Update FSBASE for the user TLS pointer (only needed once userspace
//     threads exist; t->user_tls_base will drive it).
//   - Restore FPU/SSE/AVX state — left lazy until the FPU is actually
//     touched by user code.
void arch_thread_install(struct thread *t) {
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];

  cpu_tss_set_rsp0(cs->arch_per_cpu, t->kernel_stack_top);

  // Avoid a CR3 write when the AS isn't actually changing — that flush
  // is expensive and most kernel-thread switches stay in g_as_kernel.
  if (t->proc != nullptr && t->proc->as != nullptr &&
      t->proc->as != cs->current_as) {
    as_switch(t->proc->as);
  }
}
