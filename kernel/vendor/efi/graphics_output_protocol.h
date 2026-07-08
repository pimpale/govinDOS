#ifndef EFI_GRAPHICS_OUTPUT_PROTOCOL_H
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_H

#include <efi/types.h>

// {9042A9DE-23DC-4A38-96FB-7ADED080516A}
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID                                      \
  {                                                                            \
    0x9042A9DE, 0x23DC, 0x4A38, {                                              \
      0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A                           \
    }                                                                          \
  }

// EFI_GRAPHICS_PIXEL_FORMAT
enum {
  EFI_PIXEL_RGB_RESERVED_8BPC = 0,
  EFI_PIXEL_BGR_RESERVED_8BPC = 1,
  EFI_PIXEL_BIT_MASK          = 2,
  EFI_PIXEL_BLT_ONLY          = 3,
};

struct efi_pixel_bitmask {
  uint32_t red_mask;
  uint32_t green_mask;
  uint32_t blue_mask;
  uint32_t reserved_mask;
};

struct efi_graphics_output_mode_information {
  uint32_t version;
  uint32_t horizontal_resolution;
  uint32_t vertical_resolution;
  uint32_t pixel_format; // EFI_GRAPHICS_PIXEL_FORMAT
  struct efi_pixel_bitmask pixel_information;
  uint32_t pixels_per_scan_line;
};

struct efi_graphics_output_protocol_mode {
  uint32_t max_mode;
  uint32_t mode;
  struct efi_graphics_output_mode_information *info;
  efi_uint_t size_of_info;
  efi_physical_address_t frame_buffer_base;
  efi_uint_t frame_buffer_size;
};

struct efi_graphics_output_protocol {
  void *query_mode;
  void *set_mode;
  void *blt;
  struct efi_graphics_output_protocol_mode *mode;
};

#endif // EFI_GRAPHICS_OUTPUT_PROTOCOL_H
