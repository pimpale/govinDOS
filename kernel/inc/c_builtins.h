#ifndef c_builtins_h_INCLUDED
#define c_builtins_h_INCLUDED

#include <stddef.h>

void* memset(void* ptr, int value, size_t size);
void *memcpy(void *dst, const void *src, size_t size);
void *memmove(void *dest, const void *src, size_t n);

#endif // c_builtins_h_INCLUDED
