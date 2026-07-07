#include "stacks.h"
#include "debug.h"
#include "paging.h"
#include "stdlib.h"

#define KERNEL_INTERRUPT_STACK_SIZE (16 * 1024)
#define KERNEL_BOOTSTRAP_STACK_SIZE (1024 * 1024)

static size_t stack_size_for(enum stack_type purpose) {
  switch (purpose) {
  case STACK_TYPE_KERNEL_INTERRUPT:
    return KERNEL_INTERRUPT_STACK_SIZE;
  case STACK_TYPE_KERNEL_BOOTSTRAP:
    return KERNEL_BOOTSTRAP_STACK_SIZE;
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

  // Guard punch in g_as_kernel. All of these stacks are allocated during
  // bring-up, before the first user AS is cloned, so the guards are part
  // of the boot-static skeleton every clone inherits — and they are
  // never freed, so g_as_kernel stays boot-static afterwards (which is
  // what lets user ASes clone the live kernel tree safely).
  as_flag(g_as_kernel, (uint64_t)(uintptr_t)base,
          (uint64_t)(uintptr_t)(base + PAGE_SIZE), 0);

  as_flush(g_as_kernel);

  return base + total_size;
}
