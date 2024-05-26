#ifndef kmath_h_INCLUDED
#define kmath_h_INCLUDED

#include <stdint.h>

static inline uint64_t max_u64(uint64_t a, uint64_t b) {
  if (a > b) {
    return a;
  } else {
    return b;
  }
}

static inline uint64_t min_u64(uint64_t a, uint64_t b) {
  if (a < b) {
    return a;
  } else {
    return b;
  }
}

#endif // kmath_h_INCLUDED
