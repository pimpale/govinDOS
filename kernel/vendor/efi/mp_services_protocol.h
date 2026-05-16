#ifndef EFI_MP_SERVICES_PROTOCOL_H
#define EFI_MP_SERVICES_PROTOCOL_H

#include <efi/efi.h>
#include <efi/types.h>

// {3FDDA605-A76E-4F46-AD29-12F4531B3D08}
#define EFI_MP_SERVICES_PROTOCOL_GUID                                          \
  {                                                                            \
    0x3FDDA605, 0xA76E, 0x4F46, {                                              \
      0xAD, 0x29, 0x12, 0xF4, 0x53, 0x1B, 0x3D, 0x08                           \
    }                                                                          \
  }

// StatusFlag bits returned by get_processor_info
#define PROCESSOR_AS_BSP_BIT      0x00000001
#define PROCESSOR_ENABLED_BIT     0x00000002
#define PROCESSOR_HEALTH_STATUS_BIT 0x00000004

// Sentinel "all processors except BSP" value for the processor_number
// argument to enable_disable_ap / startup_this_ap (not used by all calls).
#define END_OF_CPU_LIST           0xFFFFFFFF

// Procedure run on an AP via startup_all_aps / startup_this_ap.
typedef void (EFIAPI *efi_ap_procedure)(void *procedure_argument);

struct efi_cpu_physical_location {
  uint32_t package;
  uint32_t core;
  uint32_t thread;
};

struct efi_processor_information {
  uint64_t                         processor_id;
  uint32_t                         status_flag;
  struct efi_cpu_physical_location location;
};

struct efi_mp_services_protocol {
  efi_status_t (EFIAPI *get_number_of_processors)(
      struct efi_mp_services_protocol *this,
      efi_uint_t                      *number_of_processors,
      efi_uint_t                      *number_of_enabled_processors);

  efi_status_t (EFIAPI *get_processor_info)(
      struct efi_mp_services_protocol  *this,
      efi_uint_t                        processor_number,
      struct efi_processor_information *processor_info_buffer);

  efi_status_t (EFIAPI *startup_all_aps)(
      struct efi_mp_services_protocol *this,
      efi_ap_procedure                 procedure,
      efi_bool_t                       single_thread,
      efi_event_t                      wait_event,
      efi_uint_t                       timeout_in_microseconds,
      void                            *procedure_argument,
      efi_uint_t                     **failed_cpu_list);

  efi_status_t (EFIAPI *startup_this_ap)(
      struct efi_mp_services_protocol *this,
      efi_ap_procedure                 procedure,
      efi_uint_t                       processor_number,
      efi_event_t                      wait_event,
      efi_uint_t                       timeout_in_microseconds,
      void                            *procedure_argument,
      efi_bool_t                      *finished);

  efi_status_t (EFIAPI *switch_bsp)(
      struct efi_mp_services_protocol *this,
      efi_uint_t                       processor_number,
      efi_bool_t                       enable_old_bsp);

  efi_status_t (EFIAPI *enable_disable_ap)(
      struct efi_mp_services_protocol *this,
      efi_uint_t                       processor_number,
      efi_bool_t                       enable_ap,
      uint32_t                        *health_flag);

  efi_status_t (EFIAPI *who_am_i)(
      struct efi_mp_services_protocol *this,
      efi_uint_t                      *processor_number);
};

#endif // EFI_MP_SERVICES_PROTOCOL_H
