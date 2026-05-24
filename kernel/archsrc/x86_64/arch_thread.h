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

#endif // arch_thread_h_INCLUDED
