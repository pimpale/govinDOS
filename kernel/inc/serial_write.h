#ifndef efi_write_h_INCLUDED
#define efi_write_h_INCLUDED

#include <stdint.h>

void serial_write_string(const char *str);
void serial_write_u8buf(const uint8_t *str, uint64_t len);
void serial_write_u64hex(uint64_t v);
void serial_write_u32hex(uint32_t v);
void serial_write_u16hex(uint16_t v);
void serial_write_u8hex(uint8_t v);

#endif // efi_write_h_INCLUDED
