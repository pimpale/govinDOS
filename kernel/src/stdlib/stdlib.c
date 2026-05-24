#include "stdlib.h"

#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "debug.h"
#include "math.h"
#include "panic.h"
#include "spinlock.h"
#include "string.h"

void *malloc(size_t size) {

  spinlock_lock(&g_allocator_lock);
  void *p = malloc_unlocked(size);
  spinlock_unlock(&g_allocator_lock);

  return p;
}

void *calloc(size_t nmemb, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return nullptr;
  }

  spinlock_lock(&g_allocator_lock);
  void *p = malloc_unlocked(total);
  spinlock_unlock(&g_allocator_lock);

  if (p == nullptr) {
    return nullptr;
  }
  memset(p, 0, total);
  return p;
}

void *realloc(void *ptr, size_t size) {
  if (ptr == nullptr) {
    return malloc(size);
  }
  if (size == 0) {

    spinlock_lock(&g_allocator_lock);
    free_unlocked(ptr);
    spinlock_unlock(&g_allocator_lock);
    return nullptr;
  }

  spinlock_lock(&g_allocator_lock);

  uint64_t old_size;
  buddy_status_t s = buddy_mem_size(g_allocator, ptr, &old_size);
  asserts(s == BUDDY_STATUS_SUCCESS,
          "realloc: pointer not owned by allocator\n");

  // fast path: requested size fits in the existing bucket
  if ((uint64_t)size <= old_size) {
    spinlock_unlock(&g_allocator_lock);
    return ptr;
  }

  void *new_ptr = malloc_unlocked(size);
  spinlock_unlock(&g_allocator_lock);

  if (new_ptr == nullptr) {
    return nullptr;
  }
  memcpy(new_ptr, ptr, old_size);

  spinlock_lock(&g_allocator_lock);
  free_unlocked(ptr);
  spinlock_unlock(&g_allocator_lock);

  return new_ptr;
}

void free(void *ptr) {
  if (ptr == nullptr) {
    return;
  }

  spinlock_lock(&g_allocator_lock);
  free_unlocked(ptr);
  spinlock_unlock(&g_allocator_lock);
}

[[noreturn]] void abort(void) { panic(); }
