#ifndef arch_thread_h_INCLUDED
#define arch_thread_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

// Arch-private per-thread state. Portable code in src/thread.{h,c} treats
// this as opaque and only routes it through arch_thread_init_kernel /
// arch_thread_install / switch_context.
struct arch_thread {
  // Saved kernel stack pointer between context switches. switch_context
  // writes this on the way out and reads it on the way in. All other
  // callee-saved state lives on the stack pointed to by this RSP.
  uint64_t kernel_rsp;

  // Lazily-allocated XSAVE area for FPU/SSE/AVX state. NULL until first
  // FPU use. Size is variable (CPUID-dependent), known to arch code.
  void *xsave_area;
};

// -----------------------------------------------------------------------------
// IRQ save/restore. Inline so the scheduler hot path doesn't take call cost.
// Equivalents on aarch64 read/write DAIF.
// -----------------------------------------------------------------------------

static inline bool arch_irq_save(void) {
  uint64_t rflags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
  return (rflags & (1ull << 9)) != 0;
}

static inline void arch_irq_restore(bool was_enabled) {
  if (was_enabled) {
    __asm__ volatile("sti" : : : "memory");
  }
}

static inline void arch_irq_enable(void) {
  __asm__ volatile("sti" : : : "memory");
}

static inline void arch_irq_disable(void) {
  __asm__ volatile("cli" : : : "memory");
}

#endif // arch_thread_h_INCLUDED
