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

// Release a stack returned by stacks_alloc_kernel, given its top. Restores
// the guard page to the pristine kernel mapping (in g_as_kernel — the only
// AS the guard was ever punched in; kthread stacks are touched exclusively
// while g_as_kernel is current) before handing the block back to the buddy,
// per the pristinity invariant.
void stacks_free_kernel(void *stack_top, enum stack_type purpose);

#endif // stacks_h_INCLUDED