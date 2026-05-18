#ifndef stacks_h_INCLUDED
#define stacks_h_INCLUDED

#include <stddef.h>

enum stack_type {
  // an interrupt stack (small)
  STACK_TYPE_KERNEL_INTERRUPT,
  // the boostrap stack that will be entered between processes
  STACK_TYPE_KERNEL_BOOTSTRAP,
  // a task stack (will allocate one per task, may later be deallocated)
  STACK_TYPE_KERNEL_TASK,
};

void *stacks_alloc_kernel(enum stack_type purpose);

#endif // stacks_h_INCLUDED