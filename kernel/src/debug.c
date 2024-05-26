#include "debug.h"
#include "serial_write.h"

[[noreturn]]
static void panic() {
  while (true) {
  }
}

[[noreturn]]
void fatal(char *message) {
  serial_write_string(message);
  panic();
}

[[noreturn]] void fatal_s_u64_s(char *s1, uint64_t u1, char *s2) {
  serial_write_string(s1);
  serial_write_u64hex(u1);
  serial_write_string(s2);
  panic();
}

void assert(bool h, char *message) {
  if (!h) {
    fatal(message);
  }
}

