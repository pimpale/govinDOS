#include "stdlib.h"

#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "debug.h"
#include "math.h"
#include "panic.h"
#include "string.h"

void *malloc(size_t size) {
  allocator_require();
  if (size == 0) {
    return nullptr;
  }

  void *p;
  buddy_status_t s = buddy_mem_alloc(g_allocator, (uint64_t)size, &p);
  if (s != BUDDY_STATUS_SUCCESS) {
    return nullptr;
  }
  return p;
}

void *calloc(size_t nmemb, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return nullptr;
  }
  void *p = malloc(total);
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
    free(ptr);
    return nullptr;
  }

  allocator_require();

  uint64_t old_size;
  buddy_status_t s = buddy_mem_size(g_allocator, ptr, &old_size);
  asserts(s == BUDDY_STATUS_SUCCESS,
          "realloc: pointer not owned by allocator\n");

  // fast path: requested size fits in the existing bucket
  if ((uint64_t)size <= old_size) {
    return ptr;
  }

  void *new_ptr = malloc(size);
  if (new_ptr == nullptr) {
    return nullptr;
  }
  memcpy(new_ptr, ptr, old_size);
  free(ptr);
  return new_ptr;
}

void free(void *ptr) {
  if (ptr == nullptr) {
    return;
  }
  allocator_require();
  buddy_status_t s = buddy_mem_free(g_allocator, ptr);
  asserts(s == BUDDY_STATUS_SUCCESS, "free: pointer not owned by allocator\n");
}

[[noreturn]] void abort(void) { panic(); }
