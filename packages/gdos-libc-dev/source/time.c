#include "time.h"

#include <errno.h>
#include <gdos/sys.h>
#include <stdint.h>

// Timed waiting is SYS_FUTEX_WAIT's deadline argument (timer-design.md):
// a pure sleep parks on a private word nothing ever wakes. Until a
// kernel realtime clock exists, CLOCK_REALTIME reports the monotonic
// clock too, which keeps absolute REALTIME waits internally consistent.

int clock_gettime(clockid_t clock, struct timespec *ts) {
  if (ts == nullptr ||
      (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)) {
    errno = EINVAL;
    return -1;
  }
  uint64_t now = sys_gettime();
  ts->tv_sec = (time_t)(now / 1000000000ull);
  ts->tv_nsec = (long)(now % 1000000000ull);
  return 0;
}

// The word every sleeper compares against; never written, never woken.
static _Atomic uint32_t g_sleep_word;

int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (req == nullptr || req->tv_sec < 0 || req->tv_nsec < 0 ||
      req->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return -1;
  }
  uint64_t duration =
      (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
  uint64_t deadline = sys_gettime() + duration;
  if (deadline == 0) {
    deadline = 1; // 0 means "no deadline" to the kernel
  }
  // Spurious wakes just re-park with the same absolute deadline.
  while (sys_gettime() < deadline) {
    sys_futex_wait(&g_sleep_word, 0, nullptr, deadline);
  }
  if (rem != nullptr) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}
