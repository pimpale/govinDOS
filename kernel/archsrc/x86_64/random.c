#include "random.h"

bool random64(uint64_t *out) {
  uint32_t a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(1), "c"(0));
  if (!(c & (1u << 30))) return false;
  for (uint32_t i = 0; i < 16; i++) {
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    if (ok) return true;
  }
  return false;
}
