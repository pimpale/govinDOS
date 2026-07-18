#ifndef stdio_h_INCLUDED
#define stdio_h_INCLUDED

#include <stdarg.h>

#define EOF (-1)

// Pull in the freestanding implementation and expose its standard names.
#define PRINTF_ALIAS_STANDARD_FUNCTION_NAMES_SOFT 1
#include <printf/printf.h>

int putchar(int c);
int puts(const char *s);
void perror(const char *prefix);

#endif // stdio_h_INCLUDED
