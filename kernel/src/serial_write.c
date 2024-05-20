#include "serial_write.h"

#include <stdint.h>

#include "serial.h"

static inline uint8_t tohex(uint64_t v) {
  if (v < 10) {
    return '0' + v;
  } else {
    return 'A' + v - 10;
  }
}

void serial_write_string(const char *str) {
  for (uint64_t i = 0; str[i] != 0; i++) {
    serial_putchar((uint8_t)str[i]);
  }
}

void serial_write_u8buf(const uint8_t *str, uint64_t len) {
  for (uint64_t i = 0; i < len; i++) {
    serial_putchar(str[i]);
  }
}

void serial_write_u64hex(uint64_t v) {
  constexpr uint64_t LEN = sizeof(v)*2;
  uint8_t str[LEN + 1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = tohex(v % 16);
    v /= 16;
    i--;
  }
  serial_write_u8buf(str, LEN);
}

void serial_write_u32hex(uint32_t v) {
  constexpr uint64_t LEN = sizeof(v)*2;
  uint8_t str[LEN + 1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = tohex(v % 16);
    v /= 16;
    i--;
  }
  serial_write_u8buf(str, LEN);
}

void serial_write_u16hex(uint16_t v) {
  constexpr uint64_t LEN = sizeof(v)*2;
  uint8_t str[LEN + 1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = '0' + v % 16;
    v /= 16;
    i--;
  }
  serial_write_u8buf(str, LEN);
}

void serial_write_u8hex(uint8_t v) {
  constexpr uint64_t LEN = sizeof(v)*2;
  uint8_t str[LEN + 1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = '0' + v % 16;
    v /= 16;
    i--;
  }
  serial_write_u8buf(str, LEN);
}
