#ifndef serial_h_INCLUDED
#define serial_h_INCLUDED

#include <stdint.h>

void serial_init();

void serial_putchar(uint8_t data);

#endif // serial_h_INCLUDED
