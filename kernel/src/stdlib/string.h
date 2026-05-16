#ifndef string_h_INCLUDED
#define string_h_INCLUDED

#include <stddef.h>

void* memset(void* ptr, int value, size_t size);
void *memcpy(void *dst, const void *src, size_t size);
void *memmove(void *dest, const void *src, size_t n);

#endif // string_h_INCLUDED
