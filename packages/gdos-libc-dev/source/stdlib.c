#include "stdlib.h"

#include <stdint.h>
#include <stdatomic.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include <gdos/sys.h>

#include "string.h"

void *malloc(size_t size) {
  if (size == 0) {
    return nullptr;
  }

  uint64_t base = sys_vm_alloc(size, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(base)) {
    return nullptr;
  }
  return (void *)base;
}

void *calloc(size_t nmemb, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return nullptr;
  }

  void *ptr = malloc(total);
  if (ptr != nullptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *realloc(void *ptr, size_t size) {
  if (ptr == nullptr) {
    return malloc(size);
  }
  if (size == 0) {
    free(ptr);
    return nullptr;
  }

  uint64_t old_size = sys_vm_size((uint64_t)ptr);
  if (sys_iserr(old_size)) {
    return nullptr;
  }
  if (size <= old_size) {
    return ptr;
  }

  void *new_ptr = malloc(size);
  if (new_ptr == nullptr) {
    return nullptr;
  }
  memcpy(new_ptr, ptr, old_size);
  free(ptr);
  return new_ptr;
}

void free(void *ptr) {
  if (ptr == nullptr)
    return;
  // VM_FREE performs a bounded waiter drain and deliberately leaves the
  // allocation mapped while it returns SYSERR_AGAIN.
  while (sys_vm_free((uint64_t)ptr) == SYSERR_AGAIN)
    sys_yield();
}

static int digit_value(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z')
    return c - 'A' + 10;
  return -1;
}

static unsigned long long
parse_magnitude(const char *restrict s, char **restrict end, int base,
                unsigned long long positive_limit,
                unsigned long long negative_limit, bool *negative_out,
                bool *overflow_out) {
  const char *start = s;
  while (isspace((unsigned char)*s))
    s++;
  bool negative = false;
  if (*s == '+' || *s == '-') {
    negative = *s == '-';
    s++;
  }
  if ((base == 0 || base == 16) && s[0] == '0' &&
      (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
  } else if (base == 0) {
    base = *s == '0' ? 8 : 10;
  }
  if (base < 2 || base > 36) {
    errno = EINVAL;
    if (end != nullptr)
      *end = (char *)start;
    return 0;
  }

  unsigned long long limit = negative ? negative_limit : positive_limit;
  unsigned long long value = 0;
  bool any = false;
  bool overflow = false;
  for (;;) {
    int digit = digit_value((unsigned char)*s);
    if (digit < 0 || digit >= base)
      break;
    any = true;
    if (value > (limit - (unsigned)digit) / (unsigned)base) {
      overflow = true;
      value = limit;
    } else if (!overflow) {
      value = value * (unsigned)base + (unsigned)digit;
    }
    s++;
  }
  if (end != nullptr)
    *end = (char *)(any ? s : start);
  if (overflow)
    errno = ERANGE;
  if (negative_out != nullptr)
    *negative_out = negative;
  if (overflow_out != nullptr)
    *overflow_out = overflow;
  return value;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base) {
  bool negative = false;
  unsigned long magnitude = (unsigned long)parse_magnitude(
      s, end, base, ULONG_MAX, ULONG_MAX, &negative, nullptr);
  return negative ? 0ul - magnitude : magnitude;
}

long strtol(const char *restrict s, char **restrict end, int base) {
  bool negative = false;
  bool overflow = false;
  unsigned long long magnitude = parse_magnitude(
      s, end, base, LONG_MAX, (unsigned long long)LONG_MAX + 1, &negative,
      &overflow);
  if (overflow)
    return negative ? LONG_MIN : LONG_MAX;
  if (negative && magnitude == (unsigned long long)LONG_MAX + 1)
    return LONG_MIN;
  return negative ? -(long)magnitude : (long)magnitude;
}

unsigned long long strtoull(const char *restrict s, char **restrict end,
                            int base) {
  bool negative = false;
  unsigned long long magnitude =
      parse_magnitude(s, end, base, ULLONG_MAX, ULLONG_MAX, &negative,
                      nullptr);
  return negative ? 0ull - magnitude : magnitude;
}

long long strtoll(const char *restrict s, char **restrict end, int base) {
  bool negative = false;
  bool overflow = false;
  unsigned long long magnitude = parse_magnitude(
      s, end, base, LLONG_MAX, (unsigned long long)LLONG_MAX + 1, &negative,
      &overflow);
  if (overflow)
    return negative ? LLONG_MIN : LLONG_MAX;
  if (negative && magnitude == (unsigned long long)LLONG_MAX + 1)
    return LLONG_MIN;
  return negative ? -(long long)magnitude : (long long)magnitude;
}

int atoi(const char *s) { return (int)strtol(s, nullptr, 10); }
long atol(const char *s) { return strtol(s, nullptr, 10); }
long long atoll(const char *s) { return strtoll(s, nullptr, 10); }

int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }
long long llabs(long long value) { return value < 0 ? -value : value; }

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
  const unsigned char *bytes = base;
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const void *element = bytes + middle * size;
    int order = compare(key, element);
    if (order < 0)
      high = middle;
    else if (order > 0)
      low = middle + 1;
    else
      return (void *)element;
  }
  return nullptr;
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
  unsigned char *bytes = base;
  // A compact, allocation-free insertion sort is sufficient for the basic
  // libc surface; replace it with introsort when large user datasets matter.
  for (size_t i = 1; i < count; i++) {
    size_t j = i;
    while (j != 0 && compare(bytes + (j - 1) * size, bytes + j * size) > 0) {
      for (size_t k = 0; k < size; k++) {
        unsigned char tmp = bytes[(j - 1) * size + k];
        bytes[(j - 1) * size + k] = bytes[j * size + k];
        bytes[j * size + k] = tmp;
      }
      j--;
    }
  }
}

static _Atomic uint64_t g_rand_state = 1;

void srand(unsigned seed) {
  atomic_store_explicit(&g_rand_state, seed ? seed : 1,
                        memory_order_relaxed);
}

int rand(void) {
  uint64_t old = atomic_load_explicit(&g_rand_state, memory_order_relaxed);
  uint64_t next;
  do {
    next = old * 6364136223846793005ull + 1;
  } while (!atomic_compare_exchange_weak_explicit(
      &g_rand_state, &old, next, memory_order_relaxed,
      memory_order_relaxed));
  return (int)((next >> 33) & RAND_MAX);
}

char *getenv(const char *name) {
  (void)name;
  return nullptr;
}

[[noreturn]] void _Exit(int status) { sys_proc_exit((uint32_t)status); }

// There is no atexit registry yet, so exit and _Exit currently have the same
// process-wide semantics. Keeping the standard entry points distinct lets the
// CRT add flushing and handlers later without changing the kernel ABI.
[[noreturn]] void exit(int status) { _Exit(status); }

[[noreturn]] void abort(void) { _Exit(127); }
