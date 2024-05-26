#ifndef debug_h_INCLUDED
#define debug_h_INCLUDED

#include <stdint.h>

void fatal(char *message);
void fatal_s_u64_s(char *s1, uint64_t u1, char *s2);
void assert(bool h, char *message);

#endif // debug_h_INCLUDED
