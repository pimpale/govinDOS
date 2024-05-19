#include "serial.h"

#include <stdint.h>

inline void outb(uint16_t port, uint8_t val) {
  __asm volatile("outb %b0, %w1" ::"a"(val), "Nd"(port) : "memory");
}

inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
  return ret;
}

#define COM1 0x3F8
void serial_init() {
  outb(COM1 + 1, 0x00); // Disable interrupts
  outb(COM1 + 3, 0x80); // Enable DLAB
  outb(COM1 + 0, 0x03); // Set divisor (baud rate)
  outb(COM1 + 1, 0x00);
  outb(COM1 + 3, 0x03); // 8 bits, one stop bit, no parity
  outb(COM1 + 2, 0xC7); // enable and clear FIFO, interrupt on 14 bits available
  outb(COM1 + 4, 0x0F); // setup modem control register
  // NOTE: We skip testing via loopback
}

static int is_transmit_empty() { return inb(COM1 + 5) & 0x20; }

void serial_putchar(uint8_t a) {
  if (a == '\n') {
    while (is_transmit_empty() == 0) {
    };
    outb(COM1, '\r');
    while (is_transmit_empty() == 0) {
    };
    outb(COM1, '\n');
  } else {
    while (is_transmit_empty() == 0) {
    };
    outb(COM1, a);
  }
}
