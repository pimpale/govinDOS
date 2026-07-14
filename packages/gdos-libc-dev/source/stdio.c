#include "stdio.h"

#include <gdos/sys.h>

void putchar_(char c) {
  char str[2] = {c, 0};
  sys_debug_write(str, 1);
}
