#ifndef stacks_h_INCLUDED
#define stacks_h_INCLUDED

#include <stddef.h>

// Boot-time per-CPU kernel stacks. There are no kernel threads, so these
// (interrupt/IST + bootstrap) are the only kernel stacks that exist —
// allocated once during bring-up and never freed. Their guard punches in
// g_as_kernel are part of the boot-static kernel skeleton every user AS
// is cloned from.

enum stack_type {
  // an interrupt stack (small)
  STACK_TYPE_KERNEL_INTERRUPT,
  // the bootstrap stack each CPU's scheduler/idle loop runs on forever
  STACK_TYPE_KERNEL_BOOTSTRAP,
};

void *stacks_alloc_kernel(enum stack_type purpose);

#endif // stacks_h_INCLUDED
