#ifndef assert_h_INCLUDED
#define assert_h_INCLUDED

#include "debug.h"

#define S1(x) #x
#define S2(x) S1(x)
#define LOCATION __FILE__ " : " S2(__LINE__)

#define assert(expr) asserts(expr, LOCATION)

#endif // assert_h_INCLUDED
