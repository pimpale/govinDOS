#ifndef cpu_hwid_h_INCLUDED
#define cpu_hwid_h_INCLUDED

#include <stdint.h>

// very architecture specific
// don't use this as an index, it could be pretty large
// use cpu_state_whoami instead
uint64_t cpu_hwid();

#endif