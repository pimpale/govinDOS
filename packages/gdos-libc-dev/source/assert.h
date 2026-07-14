#ifndef assert_h_INCLUDED
#define assert_h_INCLUDED

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#include <stdlib.h>
#define assert(expr) ((expr) ? (void)0 : abort())
#endif

#endif // assert_h_INCLUDED
