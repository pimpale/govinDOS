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

  // x87/SSE state (FXSAVE64 layout, fixed 512 bytes, 16-byte aligned —
  // the TCB is page-granular from the buddy allocator, so the member
  // alignment is honored). Saved eagerly by arch_uthread_save_frame and
  // restored by uthread_resume_prepare: the kernel itself is built
  // -mgeneral-regs-only and cannot dirty FPU state, so the park/resume
  // boundary is the only place user FPU state can change hands. With
  // preemption landing at arbitrary user instructions, every register in
  // here is live — no ABI carve-out applies. AVX (XSAVE) is a future
  // upgrade; until then userspace must not use YMM+ state.
  alignas(16) uint8_t fxsave_area[512];

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
