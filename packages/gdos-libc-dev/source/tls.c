#include <stdint.h>

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
