#ifndef print_h_INCLUDED
#define print_h_INCLUDED

#include "efi/simple_text_output_protocol.h"
#include <stdint.h>

efi_status_t output_string(struct efi_simple_text_output_protocol* out, uint16_t* str);
efi_status_t output_u64hex(struct efi_simple_text_output_protocol* out, uint64_t v);
efi_status_t output_u32hex(struct efi_simple_text_output_protocol* out, uint32_t v);
efi_status_t output_u16hex(struct efi_simple_text_output_protocol* out, uint16_t v);
efi_status_t output_u8hex(struct efi_simple_text_output_protocol* out, uint8_t v);

#endif // print_h_INCLUDED
