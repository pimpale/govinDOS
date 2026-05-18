#include "stacks.h"
#include "debug.h"
#include "paging.h"
#include "stdlib.h"

#define KERNEL_INTERRUPT_STACK_SIZE (16 * 1024)
#define KERNEL_BOOTSTRAP_STACK_SIZE (1024 * 1024)
#define KERNEL_TASK_STACK_SIZE (1024 * 1024)

void *stacks_alloc_kernel(enum stack_type purpose) {
  size_t total_size = 0;
  switch (purpose) {
  case STACK_TYPE_KERNEL_INTERRUPT:
    total_size = KERNEL_INTERRUPT_STACK_SIZE;
    break;
  case STACK_TYPE_KERNEL_BOOTSTRAP:
    total_size = KERNEL_BOOTSTRAP_STACK_SIZE;
    break;
  case STACK_TYPE_KERNEL_TASK:
    total_size = KERNEL_TASK_STACK_SIZE;
    break;
  default:
    fatal("unrecognized stack type");
  };

  asserts(total_size % PAGE_SIZE == 0,
          "stack size should be a multiple of page size");
  asserts(total_size >= 2 * PAGE_SIZE,
          "stack size must include a guard page and usable stack");

  uint8_t *base = calloc(1, total_size);
  asserts(base != nullptr, "failed to allocate kernel stack");

  as_flag(g_as_kernel, (uint64_t)(uintptr_t)base,
          (uint64_t)(uintptr_t)(base + PAGE_SIZE), 0);

  return base + total_size;
}