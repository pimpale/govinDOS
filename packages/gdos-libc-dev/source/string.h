#ifndef string_h_INCLUDED
#define string_h_INCLUDED

#include <stddef.h>

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *restrict dst, const char *restrict src);
char *strncpy(char *restrict dst, const char *restrict src, size_t n);
char *strcat(char *restrict dst, const char *restrict src);
char *strncat(char *restrict dst, const char *restrict src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strtok_r(char *restrict s, const char *restrict delim,
               char **restrict saveptr);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strerror(int error);
void *memset(void *ptr, int value, size_t size);
void *memcpy(void *dst, const void *src, size_t size);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *ptr, int value, size_t n);

#endif // string_h_INCLUDED
