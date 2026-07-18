#include "stdio.h"

#include <gdos/sys.h>

#include <errno.h>
#include <string.h>

void putchar_(char c) {
  char str[2] = {c, 0};
  sys_debug_write(str, 1);
}

int putchar(int c) {
  char byte = (char)c;
  return sys_debug_write(&byte, 1) == 0 ? (unsigned char)byte : EOF;
}

int puts(const char *s) {
  if (sys_debug_write(s, strlen(s)) != 0 || sys_debug_write("\n", 1) != 0)
    return EOF;
  return 0;
}

void perror(const char *prefix) {
  if (prefix != nullptr && *prefix != '\0') {
    sys_debug_write(prefix, strlen(prefix));
    sys_debug_write(": ", 2);
  }
  const char *message = strerror(errno);
  sys_debug_write(message, strlen(message));
  sys_debug_write("\n", 1);
}
