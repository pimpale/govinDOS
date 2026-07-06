#ifndef arch_thread_h_INCLUDED
#define arch_thread_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "trap_frame.h"

// Arch-private per-thread state. Portable code in src/thread.{h,c} treats
// this as opaque and only routes it through arch_thread_init_kernel /
// arch_thread_install / switch_context.
struct arch_thread {
  // Saved kernel stack pointer between context switches. switch_context
  // writes this on the way out and reads it on the way in. For kernel
  // threads all other callee-saved state lives on the stack this points
  // to; for user threads it always points at the forged frame inside
  // resume_stack below.
  uint64_t kernel_rsp;

  // Lazily-allocated XSAVE area for FPU/SSE/AVX state. NULL until first
  // FPU use. Size is variable (CPUID-dependent), known to arch code.
  void *xsave_area;

  // --- user threads only (kernel threads leave these zeroed) ---

  // Saved user-mode register context. A user thread that is not currently
  // running lives entirely in this frame: parking copies the live trap
  // frame here, uthread_resume irets from here. There is no per-thread
  // kernel stack.
  struct trap_frame uframe;

  // Tiny kernel stack used only by the uthread_resume trampoline: holds
  // the forged switch_context frame at its top and gives the trampoline
  // room for one C call (uthread_resume_prepare) before it irets. Never
  // holds state across a context switch.
  alignas(16) uint8_t resume_stack[512];
};

#endif // arch_thread_h_INCLUDED
