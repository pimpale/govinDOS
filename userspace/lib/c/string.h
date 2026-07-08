#ifndef string_h_INCLUDED
#define string_h_INCLUDED

// lib/c: the minimal libc — generic C runtime for first-party code, no
// POSIX, and no knowledge of the OS interface (that's lib/sys; the
// POSIX compatibility layer, when it exists, is lib/posix). Freestanding
// throughout: -fno-builtin, so none of these are magic to the compiler.

#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#endif // string_h_INCLUDED
