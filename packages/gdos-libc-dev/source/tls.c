#include <stdint.h>

#include <gdos/sys.h>

#include "libc_internal.h"
#include "string.h"

// Clang's x86_64 Windows TLS sequence indexes a loader-provided vector with
// this image-global module index. GovindOS static executables are one module,
// so every image uses slot zero.
uint32_t _tls_index = 0;

// lld orders bare compiler-emitted `.tls$` contributions after `.tls` and
// before `.tls$ZZZ`. These markers therefore bound the complete static TLS
// template, including initialized `_Thread_local` definitions from users.
__declspec(allocate(".tls")) char _tls_start[8] = {0};
__declspec(allocate(".tls$ZZZ")) char _tls_end[8] = {0};

struct image_tls_directory64 {
  uint64_t start_address_of_raw_data;
  uint64_t end_address_of_raw_data;
  uint64_t address_of_index;
  uint64_t address_of_callbacks;
  uint32_t size_of_zero_fill;
  uint32_t characteristics;
};

static_assert(sizeof(struct image_tls_directory64) == 40,
              "PE32+ TLS directory size");

// `_tls_used` is the PE linker convention. The build also passes
// /include:_tls_used so this archive member and directory are retained even
// in images that do not yet declare a user TLS variable.
__declspec(allocate(".rdata$T")) const struct image_tls_directory64 _tls_used = {
    .start_address_of_raw_data = (uint64_t)&_tls_start,
    .end_address_of_raw_data = (uint64_t)&_tls_end,
    .address_of_index = (uint64_t)&_tls_index,
};

#define PE_TEB_STACK_BASE_OFFSET 0x08u
#define PE_TEB_STACK_LIMIT_OFFSET 0x10u
#define PE_TEB_SELF_OFFSET 0x30u
#define PE_TEB_TLS_VECTOR_OFFSET 0x58u
#define PE_TLS_VECTOR_OFFSET 0x80u
#define PE_TLS_DATA_OFFSET 0x100u

uint64_t __gdos_tls_create(uint64_t stack_base, uint64_t stack_bytes,
                           uint64_t guard_bytes) {
  uint64_t raw_bytes = (uint64_t)(uintptr_t)_tls_end -
                       (uint64_t)(uintptr_t)_tls_start;
  uint64_t runtime_bytes;
  if (__builtin_add_overflow((uint64_t)PE_TLS_DATA_OFFSET, raw_bytes,
                             &runtime_bytes)) {
    return 0;
  }
  uint64_t runtime =
      sys_vm_alloc(runtime_bytes, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(runtime)) {
    return 0;
  }

  uint64_t vector = runtime + PE_TLS_VECTOR_OFFSET;
  uint64_t data = runtime + PE_TLS_DATA_OFFSET;
  *(uint64_t *)(runtime + PE_TEB_STACK_BASE_OFFSET) =
      stack_base + stack_bytes;
  *(uint64_t *)(runtime + PE_TEB_STACK_LIMIT_OFFSET) =
      stack_base + guard_bytes;
  *(uint64_t *)(runtime + PE_TEB_SELF_OFFSET) = runtime;
  *(uint64_t *)(runtime + PE_TEB_TLS_VECTOR_OFFSET) = vector;
  *(uint64_t *)vector = data;
  memcpy((void *)data, _tls_start, raw_bytes);
  return runtime;
}
