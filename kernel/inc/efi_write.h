#ifndef efi_write_INCLUDED
#define efi_write_INCLUDED

#include "efi/simple_text_output_protocol.h"
#include <stdint.h>

efi_status_t efi_write_string(struct efi_simple_text_output_protocol *out,
                              uint16_t *str);
efi_status_t efi_write_u64hex(struct efi_simple_text_output_protocol *out,
                              uint64_t v);
efi_status_t efi_write_u32hex(struct efi_simple_text_output_protocol *out,
                              uint32_t v);
efi_status_t efi_write_u16hex(struct efi_simple_text_output_protocol *out,
                              uint16_t v);
efi_status_t efi_write_u8hex(struct efi_simple_text_output_protocol *out,
                             uint8_t v);

#endif // efi_write_INCLUDED
