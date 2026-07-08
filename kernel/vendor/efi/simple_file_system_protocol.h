#ifndef EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_H
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_H

#include <efi/efi.h>
#include <efi/types.h>

// {964E5B22-6459-11D2-8E39-00A0C969723B}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID                                   \
  {                                                                            \
    0x964E5B22, 0x6459, 0x11D2, {                                              \
      0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B                           \
    }                                                                          \
  }

// {09576E92-6D3F-11D2-8E39-00A0C969723B}
#define EFI_FILE_INFO_GUID                                                     \
  {                                                                            \
    0x09576E92, 0x6D3F, 0x11D2, {                                              \
      0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B                           \
    }                                                                          \
  }

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

struct efi_file_protocol;

struct efi_simple_file_system_protocol {
  uint64_t revision;
  efi_status_t (EFIAPI *open_volume)(
      struct efi_simple_file_system_protocol *self,
      struct efi_file_protocol **root);
};

// Function pointers must follow the UEFI spec layout exactly; slots we don't
// call are kept as void* so ordering is preserved without dragging in every
// dependent type.
struct efi_file_protocol {
  uint64_t revision;
  efi_status_t (EFIAPI *open)(struct efi_file_protocol *self,
                              struct efi_file_protocol **new_handle,
                              const efi_char16_t *file_name,
                              uint64_t open_mode, uint64_t attributes);
  efi_status_t (EFIAPI *close)(struct efi_file_protocol *self);
  void *delete_;
  efi_status_t (EFIAPI *read)(struct efi_file_protocol *self,
                              efi_uint_t *buffer_size, void *buffer);
  void *write;
  void *get_position;
  void *set_position;
  efi_status_t (EFIAPI *get_info)(struct efi_file_protocol *self,
                                  struct efi_guid *information_type,
                                  efi_uint_t *buffer_size, void *buffer);
  void *set_info;
  void *flush;
};

// EFI_FILE_INFO, returned by get_info(EFI_FILE_INFO_GUID). The trailing
// file name is variable length; only `file_size` is read here.
struct efi_file_info {
  uint64_t size;
  uint64_t file_size;
  uint64_t physical_size;
  uint8_t  create_time[16];      // EFI_TIME, opaque here
  uint8_t  last_access_time[16];
  uint8_t  modification_time[16];
  uint64_t attribute;
  efi_char16_t file_name[];
};

#endif // EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_H
