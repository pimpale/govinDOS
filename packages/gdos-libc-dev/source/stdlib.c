#include "stdlib.h"

#include <stdint.h>

#include <gdos/sys.h>

#include "string.h"

void *malloc(size_t size) {
  if (size == 0) {
    return nullptr;
  }

  uint64_t base = sys_vm_alloc(size, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(base)) {
    return nullptr;
  }
  return (void *)base;
}

void *calloc(size_t nmemb, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return nullptr;
  }

  void *ptr = malloc(total);
  if (ptr != nullptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *realloc(void *ptr, size_t size) {
  if (ptr == nullptr) {
    return malloc(size);
  }
  if (size == 0) {
    free(ptr);
    return nullptr;
  }

  uint64_t old_size = sys_vm_size((uint64_t)ptr);
  if (sys_iserr(old_size)) {
    return nullptr;
  }
  if (size <= old_size) {
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
  if (ptr != nullptr) {
    sys_vm_free((uint64_t)ptr);
  }
}

[[noreturn]] void abort(void) { sys_exit(); }
