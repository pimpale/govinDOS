#ifndef stdlib_h_INCLUDED
#define stdlib_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

[[nodiscard]] void *malloc(size_t size);
[[nodiscard]] void *calloc(size_t nmemb, size_t size);
[[nodiscard]] void *realloc(void *ptr, size_t size);
void free(void *ptr);

[[noreturn]] void abort(void);

#endif // stdlib_h_INCLUDED
