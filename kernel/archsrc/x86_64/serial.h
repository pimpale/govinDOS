#ifndef serial_h_INCLUDED
#define serial_h_INCLUDED

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
  __asm volatile("outb %b0, %w1" ::"a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
  return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
  __asm volatile("outl %0, %w1" ::"a"(val), "Nd"(port) : "memory");
}

void serial_init();

void serial_putchar(uint8_t data);

#endif // serial_h_INCLUDED
