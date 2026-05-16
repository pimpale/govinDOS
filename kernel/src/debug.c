#include "debug.h"

#include "panic.h"
#include "stdlib/stdio.h"

[[noreturn]]
void fatal(char *message) {
  printf("%s", message);
  panic();
}

[[noreturn]] void fatal_s_u64_s(char *s1, uint64_t u1, char *s2) {
  printf("%s%016llX%s", s1, u1, s2);
  panic();
}

void assert(bool h, char *message) {
  if (!h) {
    fatal(message);
  }
}
