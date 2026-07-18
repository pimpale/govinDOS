#ifndef libc_internal_h_INCLUDED
#define libc_internal_h_INCLUDED

#include <stdint.h>

// Construct a PE-compatible TLS/TEB block for this static image. The returned
// value is the GSBASE passed to SYS_THREAD_SPAWN and is freed with free().
uint64_t __gdos_tls_create(uint64_t stack_base, uint64_t stack_bytes,
                           uint64_t guard_bytes);

#endif // libc_internal_h_INCLUDED
