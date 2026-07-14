#ifndef string_h_INCLUDED
#define string_h_INCLUDED

#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memset(void *ptr, int value, size_t size);
void *memcpy(void *dst, const void *src, size_t size);
void *memmove(void *dest, const void *src, size_t n);

#endif // string_h_INCLUDED
