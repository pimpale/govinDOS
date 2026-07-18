#ifndef time_h_INCLUDED
#define time_h_INCLUDED

#include <stdint.h>

typedef int64_t time_t;
typedef int clockid_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

#endif // time_h_INCLUDED
