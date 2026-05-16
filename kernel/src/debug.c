#include "debug.h"

#include "panic.h"
#include "stdlib/stdio.h"

[[noreturn]]
void fatal(char *message) {
  printf("%s", message);
  panic();
}

void asserts(bool h, char *message) {
  if (!h) {
    fatal(message);
  }
}
