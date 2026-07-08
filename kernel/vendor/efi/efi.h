#ifndef EFI_EFI_H
#define EFI_EFI_H

#include <efi/types.h>

// --- Memory types (EFI_MEMORY_TYPE) ---------------------------------------
enum {
  EFI_RESERVED_MEMORY_TYPE        = 0,
  EFI_LOADER_CODE                 = 1,
  EFI_LOADER_DATA                 = 2,
  EFI_BOOT_SERVICES_CODE          = 3,
  EFI_BOOT_SERVICES_DATA          = 4,
  EFI_RUNTIME_SERVICES_CODE       = 5,
  EFI_RUNTIME_SERVICES_DATA       = 6,
  EFI_CONVENTIONAL_MEMORY         = 7,
  EFI_UNUSABLE_MEMORY             = 8,
  EFI_ACPI_RECLAIM_MEMORY         = 9,
  EFI_ACPI_MEMORY_NVS             = 10,
  EFI_MEMORY_MAPPED_IO            = 11,
  EFI_MEMORY_MAPPED_IO_PORT_SPACE = 12,
  EFI_PAL_CODE                    = 13,
  EFI_PERSISTENT_MEMORY           = 14,
  EFI_MAX_MEMORY_TYPE             = 15,
};

// --- AllocatePages allocation type ----------------------------------------
enum {
  EFI_ALLOCATE_ANY_PAGES   = 0,
  EFI_ALLOCATE_MAX_ADDRESS = 1,
  EFI_ALLOCATE_ADDRESS     = 2,
};

// --- Table header ---------------------------------------------------------
struct efi_table_header {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t crc32;
  uint32_t reserved;
};

// --- Memory descriptor ----------------------------------------------------
// The on-the-wire stride is reported by desc_size from get_memory_map and
// may exceed sizeof(this struct) on future spec revisions.
struct efi_memory_descriptor {
  uint32_t               type;
  uint32_t               _pad;
  efi_physical_address_t physical_start;
  efi_virtual_address_t  virtual_start;
  uint64_t               pages;
  uint64_t               attributes;
};

struct efi_system_table;

// --- Boot services table --------------------------------------------------
// Function pointers must follow the UEFI spec layout exactly; slots we don't
// call are kept as void* so ordering is preserved without dragging in every
// dependent type.
struct efi_boot_services {
  struct efi_table_header hdr;

  // Task Priority Services
  void *raise_tpl;
  void *restore_tpl;

  // Memory Services
  efi_status_t (EFIAPI *allocate_pages)(efi_uint_t type,
                                        efi_uint_t memory_type,
                                        efi_uint_t pages,
                                        efi_physical_address_t *memory);
  efi_status_t (EFIAPI *free_pages)(efi_physical_address_t memory,
                                    efi_uint_t pages);
  efi_status_t (EFIAPI *get_memory_map)(efi_uint_t *mmap_size,
                                        struct efi_memory_descriptor *mmap,
                                        efi_uint_t *map_key,
                                        efi_uint_t *desc_size,
                                        uint32_t *desc_version);
  efi_status_t (EFIAPI *allocate_pool)(efi_uint_t pool_type,
                                       efi_uint_t size,
                                       void **buffer);
  efi_status_t (EFIAPI *free_pool)(void *buffer);

  // Event & Timer Services
  void *create_event;
  void *set_timer;
  void *wait_for_event;
  void *signal_event;
  void *close_event;
  void *check_event;

  // Protocol Handler Services
  void *install_protocol_interface;
  void *reinstall_protocol_interface;
  void *uninstall_protocol_interface;
  efi_status_t (EFIAPI *handle_protocol)(efi_handle_t handle,
                                         struct efi_guid *protocol,
                                         void **interface);
  void *_reserved_proto;
  void *register_protocol_notify;
  void *locate_handle;
  void *locate_device_path;
  void *install_configuration_table;

  // Image Services
  void *load_image;
  void *start_image;
  void *exit;
  void *unload_image;
  efi_status_t (EFIAPI *exit_boot_services)(efi_handle_t image_handle,
                                            efi_uint_t map_key);

  // Misc Services
  void *get_next_monotonic_count;
  void *stall;
  void *set_watchdog_timer;

  // DriverSupport Services (UEFI 1.1+)
  void *connect_controller;
  void *disconnect_controller;

  // Open/Close Protocol Services (UEFI 1.1+)
  void *open_protocol;
  void *close_protocol;
  void *open_protocol_information;

  // Library Services (UEFI 1.1+)
  void *protocols_per_handle;
  void *locate_handle_buffer;
  efi_status_t (EFIAPI *locate_protocol)(struct efi_guid *protocol,
                                         void *registration,
                                         void **interface);
  void *install_multiple_protocol_interfaces;
  void *uninstall_multiple_protocol_interfaces;

  // 32-bit CRC Services
  void *calculate_crc32;

  // Misc Services (UEFI 1.1+)
  void *copy_mem;
  void *set_mem;
  void *create_event_ex;
};

// --- Runtime services (opaque to us for now) ------------------------------
struct efi_runtime_services {
  struct efi_table_header hdr;
};

// --- Simple text I/O (opaque) ---------------------------------------------
struct efi_simple_text_input_protocol;
struct efi_simple_text_output_protocol;

// --- Configuration table --------------------------------------------------
struct efi_configuration_table {
  struct efi_guid vendor_guid;
  void           *vendor_table;
};

// --- System table ---------------------------------------------------------
struct efi_system_table {
  struct efi_table_header  hdr;
  efi_char16_t            *firmware_vendor;
  uint32_t                 firmware_revision;
  efi_handle_t             console_in_handle;
  struct efi_simple_text_input_protocol  *con_in;
  efi_handle_t             console_out_handle;
  struct efi_simple_text_output_protocol *con_out;
  efi_handle_t             standard_error_handle;
  struct efi_simple_text_output_protocol *std_err;
  struct efi_runtime_services            *runtime;
  struct efi_boot_services               *boot;
  efi_uint_t               number_of_table_entries;
  struct efi_configuration_table         *configuration_table;
};

#endif // EFI_EFI_H
