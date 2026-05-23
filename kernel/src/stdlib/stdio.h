#ifndef stdio_h_INCLUDED
#define stdio_h_INCLUDED

#include <stdarg.h>

// Pulls in printf_ / vprintf_ from the vendor library. The SOFT alias
// (printf -> printf_) is defined inside; we undef it below and replace
// it with our own locking wrapper so concurrent SMP printf calls don't
// interleave on the serial console.
#define PRINTF_ALIAS_STANDARD_FUNCTION_NAMES_SOFT 1
#include <printf/printf.h>

#undef printf
#undef vprintf

// Underscored attribute names (__format__, __printf__) are immune to
// macro substitution from any printf-related #define above.
int printf_locked(const char *fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)));
int vprintf_locked(const char *fmt, va_list args);

#define printf(...) printf_locked(__VA_ARGS__)
#define vprintf(fmt, args) vprintf_locked((fmt), (args))

#endif // stdio_h_INCLUDED
