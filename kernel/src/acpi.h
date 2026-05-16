#ifndef acpi_h_INCLUDED
#define acpi_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include <efi/efi.h>

// --- Raw ACPI structures --------------------------------------------------

// Root System Description Pointer. The first 20 bytes are the ACPI 1.0 layout;
// fields after `revision` only exist when revision >= 2.
struct acpi_rsdp {
  char     signature[8];   // "RSD PTR "
  uint8_t  checksum;       // sum of first 20 bytes must be 0
  char     oem_id[6];
  uint8_t  revision;       // 0 = ACPI 1.0, 2 = ACPI 2.0+
  uint32_t rsdt_address;
  // ACPI 2.0+ only:
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t  extended_checksum; // sum of `length` bytes must be 0
  uint8_t  reserved[3];
} __attribute__((packed));

// Header shared by every System Description Table (RSDT, XSDT, MADT, etc).
struct acpi_sdt_header {
  char     signature[4];
  uint32_t length;
  uint8_t  revision;
  uint8_t  checksum;
  char     oem_id[6];
  char     oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

// Multiple APIC Description Table ("APIC" signature).
struct acpi_madt {
  struct acpi_sdt_header header;
  uint32_t local_apic_address;
  uint32_t flags;            // bit 0 = PCAT_COMPAT (legacy 8259 PIC present)
  // Followed by `length - sizeof(*this)` bytes of variable entries; each
  // entry begins with struct acpi_madt_entry_header.
} __attribute__((packed));

// Header on every MADT entry (ICS).
struct acpi_madt_entry_header {
  uint8_t type;
  uint8_t length;
} __attribute__((packed));

enum acpi_madt_entry_type {
  ACPI_MADT_LOCAL_APIC               = 0,
  ACPI_MADT_IO_APIC                  = 1,
  ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE = 2,
  ACPI_MADT_LAPIC_ADDRESS_OVERRIDE   = 5,
  ACPI_MADT_LOCAL_X2APIC             = 9,
};

// Type 0
struct acpi_madt_local_apic {
  struct acpi_madt_entry_header header;
  uint8_t  acpi_processor_id;
  uint8_t  apic_id;
  uint32_t flags;            // bit 0 = enabled, bit 1 = online_capable
} __attribute__((packed));

// Type 5 (overrides MADT.local_apic_address)
struct acpi_madt_lapic_address_override {
  struct acpi_madt_entry_header header;
  uint16_t reserved;
  uint64_t local_apic_address;
} __attribute__((packed));

// --- Friendly representation ----------------------------------------------

#define ACPI_MAX_PROCESSORS 255

struct acpi_processor {
  uint8_t acpi_processor_id;
  uint8_t apic_id;
  bool    enabled;         // OS may use this CPU now
  bool    online_capable;  // OS may bring it online later
};

struct acpi_processor_list {
  uint64_t local_apic_address; // honors a Type 5 override if present
  uint32_t madt_flags;         // raw MADT flags (bit 0 = legacy PIC present)
  size_t   count;
  // Type 9 (x2APIC) entries are skipped — APIC IDs > 254 don't fit here.
  struct acpi_processor processors[ACPI_MAX_PROCESSORS];
};

// --- API -------------------------------------------------------------------

// Locates the RSDP via the EFI configuration table. Must be called before
// exit_boot_services so the system table is still valid. The RSDP itself
// lives in EfiACPIReclaimMemory which UEFI preserves across exit_boot_services,
// so the returned pointer remains valid afterwards. Returns nullptr if the
// RSDP is missing or fails validation.
const struct acpi_rsdp *acpi_init(struct efi_system_table *system);

// Walks the RSDT or XSDT (XSDT preferred when rsdp->revision >= 2) looking
// for the MADT. Returns nullptr if not present or invalid.
const struct acpi_madt *acpi_find_madt(const struct acpi_rsdp *rsdp);

// Parses the MADT into a friendly processor list. Caller owns the returned
// value (returned by value, ~1 KB).
struct acpi_processor_list acpi_parse_processors(const struct acpi_madt *madt);

#endif // acpi_h_INCLUDED
