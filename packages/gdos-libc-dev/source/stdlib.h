#ifndef stdlib_h_INCLUDED
#define stdlib_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

[[nodiscard]] void *malloc(size_t size);
[[nodiscard]] void *calloc(size_t nmemb, size_t size);
[[nodiscard]] void *realloc(void *ptr, size_t size);
void free(void *ptr);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
long strtol(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
long long strtoll(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end,
                            int base);

int abs(int value);
long labs(long value);
long long llabs(long long value);

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));

int rand(void);
void srand(unsigned seed);
char *getenv(const char *name);

[[noreturn]] void abort(void);
[[noreturn]] void _Exit(int status);
[[noreturn]] void exit(int status);

#endif // stdlib_h_INCLUDED
