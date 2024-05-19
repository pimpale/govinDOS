#include "debug.h"
#include "serial_write.h"

void assert(bool h, char *message) {
  if (!h) {
    serial_write_string(message);
    while (true) {
    }
  }
}
