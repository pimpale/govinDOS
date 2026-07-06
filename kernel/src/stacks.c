#include "stacks.h"
#include "debug.h"
#include "paging.h"
#include "stdlib.h"

#define KERNEL_INTERRUPT_STACK_SIZE (16 * 1024)
#define KERNEL_BOOTSTRAP_STACK_SIZE (1024 * 1024)
#define KERNEL_TASK_STACK_SIZE (1024 * 1024)

static size_t stack_size_for(enum stack_type purpose) {
  switch (purpose) {
  case STACK_TYPE_KERNEL_INTERRUPT:
    return KERNEL_INTERRUPT_STACK_SIZE;
  case STACK_TYPE_KERNEL_BOOTSTRAP:
    return KERNEL_BOOTSTRAP_STACK_SIZE;
  case STACK_TYPE_KERNEL_TASK:
    return KERNEL_TASK_STACK_SIZE;
  default:
    fatal("unrecognized stack type");
  }
}

void *stacks_alloc_kernel(enum stack_type purpose) {
  size_t total_size = stack_size_for(purpose);

  asserts(total_size % PAGE_SIZE == 0,
          "stack size should be a multiple of page size");
  asserts(total_size >= 2 * PAGE_SIZE,
          "stack size must include a guard page and usable stack");

  uint8_t *base = calloc(1, total_size);
  asserts(base != nullptr, "failed to allocate kernel stack");

  // Guard punches go to g_as_kernel only. Per-CPU (interrupt/bootstrap)
  // stacks are allocated before g_as_template is sealed, so their guards
  // land in every later clone; task stacks are only ever touched while
  // g_as_kernel is current (scheduler CR3 policy), so theirs need not.
  as_flag(g_as_kernel, (uint64_t)(uintptr_t)base,
          (uint64_t)(uintptr_t)(base + PAGE_SIZE), 0);

  as_flush(g_as_kernel);

  return base + total_size;
}

void stacks_free_kernel(void *stack_top, enum stack_type purpose) {
  asserts(g_as_template == nullptr || purpose == STACK_TYPE_KERNEL_TASK,
          "stacks: per-CPU stacks are never freed post-seal");
  size_t total_size = stack_size_for(purpose);
  uint8_t *base = (uint8_t *)stack_top - total_size;

  // Pristinity invariant: restore the guard page to the boot mapping and
  // flush before the buddy can recycle the block. The merge pass folds the
  // fragmented tables back into the surrounding hugepage.
  as_flag(g_as_kernel, (uint64_t)(uintptr_t)base,
          (uint64_t)(uintptr_t)(base + PAGE_SIZE), PAGE_KERNEL_PRISTINE);
  as_flush(g_as_kernel);

  free(base);
}
