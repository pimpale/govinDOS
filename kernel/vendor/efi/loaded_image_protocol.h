#ifndef EFI_LOADED_IMAGE_PROTOCOL_H
#define EFI_LOADED_IMAGE_PROTOCOL_H

#include <efi/efi.h>
#include <efi/types.h>

// {5B1B31A1-9562-11D2-8E3F-00A0C969723B}
#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                         \
  {                                                                            \
    0x5B1B31A1, 0x9562, 0x11D2, {                                              \
      0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B                           \
    }                                                                          \
  }

#define EFI_LOADED_IMAGE_PROTOCOL_REVISION 0x1000

struct efi_device_path_protocol;

struct efi_loaded_image_protocol {
  uint32_t                         revision;
  efi_handle_t                     parent_handle;
  struct efi_system_table         *system_table;

  // Source location of the image
  efi_handle_t                     device_handle;
  struct efi_device_path_protocol *file_path;
  void                            *reserved;

  // Load options passed to the image
  uint32_t                         load_options_size;
  void                            *load_options;

  // Location where the image was loaded
  void                            *image_base;
  uint64_t                         image_size;
  efi_uint_t                       image_code_type;
  efi_uint_t                       image_data_type;

  efi_status_t (EFIAPI *unload)(efi_handle_t image_handle);
};

#endif // EFI_LOADED_IMAGE_PROTOCOL_H
