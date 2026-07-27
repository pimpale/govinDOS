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

// CLOCK_REALTIME currently reports the monotonic clock too: no kernel
// realtime clock exists yet (timer-design.md).
int clock_gettime(clockid_t clock, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif // time_h_INCLUDED
