#ifndef EFI_TYPES_H
#define EFI_TYPES_H

#include <stddef.h>
#include <stdint.h>

// Compiled with -target x86_64-unknown-windows, so MS x64 calling convention
// is the default. EFIAPI is kept as a hook for non-windows targets.
#ifndef EFIAPI
#define EFIAPI
#endif

typedef uint8_t  efi_bool_t;
typedef uint16_t efi_char16_t;
typedef uintptr_t efi_uint_t;    // UINTN (native unsigned)
typedef intptr_t  efi_int_t;     // INTN  (native signed)
typedef efi_uint_t efi_status_t; // EFI_STATUS (high bit = error on UINTN)
typedef void *efi_handle_t;
typedef void *efi_event_t;
typedef uint64_t efi_physical_address_t;
typedef uint64_t efi_virtual_address_t;

struct efi_guid {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  uint8_t  data4[8];
};

// EFI_STATUS error encoding: high bit of a UINTN set
#define EFI_ERR_BIT (((efi_status_t)1) << (sizeof(efi_status_t) * 8 - 1))
#define EFIERR(x)   (EFI_ERR_BIT | (efi_status_t)(x))

#define EFI_SUCCESS               ((efi_status_t)0)
#define EFI_LOAD_ERROR            EFIERR(1)
#define EFI_INVALID_PARAMETER     EFIERR(2)
#define EFI_UNSUPPORTED           EFIERR(3)
#define EFI_BAD_BUFFER_SIZE       EFIERR(4)
#define EFI_BUFFER_TOO_SMALL      EFIERR(5)
#define EFI_NOT_READY             EFIERR(6)
#define EFI_DEVICE_ERROR          EFIERR(7)
#define EFI_WRITE_PROTECTED       EFIERR(8)
#define EFI_OUT_OF_RESOURCES      EFIERR(9)
#define EFI_NOT_FOUND             EFIERR(14)

#endif // EFI_TYPES_H
