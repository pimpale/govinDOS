#include "stdio.h"

#include <stdint.h>

#include "serial.h"

void putchar_(char c) {
  serial_putchar((uint8_t)c);
}
