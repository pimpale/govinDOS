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

  // Eager standard-format XSAVE state. The allocation is dynamically sized
  // from CPUID.0D and the usable address is 64-byte aligned as XSAVE requires.
  // The kernel is -mgeneral-regs-only, so only user execution can dirty it.
  void *xsave_allocation;
  void *xsave_area;

  // Both user TLS bases are architectural thread state. ELF/SysV runtimes
  // conventionally use FS; PE/Win64 runtimes conventionally use GS.
  uint64_t user_fs_base;
  uint64_t user_gs_base;

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

struct thread;

void arch_thread_destroy(struct thread *t);
uint64_t arch_uthread_set_bases(struct thread *t, uint64_t fs_base,
                                uint64_t gs_base);

#endif // arch_thread_h_INCLUDED
