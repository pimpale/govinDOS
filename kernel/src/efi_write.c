#include "efi_write.h"

#include <stdint.h>

static inline uint16_t tohex(uint64_t v) {
  if (v < 10) {
    return '0' + v;
  } else {
    return 'A' + v - 10;
  }
}

efi_status_t efi_write_string(struct efi_simple_text_output_protocol *out,
                           uint16_t *str) {
  return out->output_string(out, str);
}

efi_status_t efi_write_u64hex(struct efi_simple_text_output_protocol *out,
                           uint64_t v) {
  constexpr uint64_t LEN = sizeof(v) * 2;
  uint16_t str[LEN+1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = tohex(v % 16);
    v /= 16;
    i--;
  }
  return out->output_string(out, str);
}

efi_status_t efi_write_u32hex(struct efi_simple_text_output_protocol *out,
                           uint32_t v) {
  constexpr uint64_t LEN = sizeof(v) * 2;
  uint16_t str[LEN+1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = tohex(v % 16);
    v /= 16;
    i--;
  }
  return out->output_string(out, str);
}

efi_status_t efi_write_u16hex(struct efi_simple_text_output_protocol *out,
                           uint16_t v) {
  constexpr uint64_t LEN = sizeof(v) * 2;
  uint16_t str[LEN+1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = '0' + v % 16;
    v /= 16;
    i--;
  }
  return out->output_string(out, str);
}

efi_status_t efi_write_u8hex(struct efi_simple_text_output_protocol *out,
                          uint8_t v) {
  constexpr uint64_t LEN = sizeof(v) * 2;
  uint16_t str[LEN+1] = {};
  for (uint32_t i = 0; i < LEN; i++) {
    str[i] = '0';
  }

  uint32_t i = LEN - 1;

  while (v > 0) {
    str[i] = '0' + v % 16;
    v /= 16;
    i--;
  }
  return out->output_string(out, str);
}
