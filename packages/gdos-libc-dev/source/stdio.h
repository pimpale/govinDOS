#ifndef stdio_h_INCLUDED
#define stdio_h_INCLUDED

#include <stdarg.h>

// Pull in the freestanding implementation and expose its standard names.
#define PRINTF_ALIAS_STANDARD_FUNCTION_NAMES_SOFT 1
#include <printf/printf.h>

#endif // stdio_h_INCLUDED
