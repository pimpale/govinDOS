#include "print.h"
#include <stdint.h>

uint16_t tohex(uint64_t v) {
  if (v < 10) {
    return '0' + v;
  } else {
    return 'A' + v - 10;
  }
}

efi_status_t output_string(struct efi_simple_text_output_protocol *out,
                           uint16_t *str) {
  return out->output_string(out, str);
}

efi_status_t output_u64hex(struct efi_simple_text_output_protocol *out,
                           uint64_t v) {
  constexpr int LEN = 16;
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

efi_status_t output_u32hex(struct efi_simple_text_output_protocol *out,
                           uint32_t v) {
  constexpr int LEN = 8;
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
efi_status_t output_u16hex(struct efi_simple_text_output_protocol *out,
                           uint16_t v) {
  constexpr int LEN = 4;
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

efi_status_t output_u8hex(struct efi_simple_text_output_protocol *out,
                          uint8_t v) {
  constexpr int LEN = 2;
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
