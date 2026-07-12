#ifndef acpi_h_INCLUDED
#define acpi_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include <efi/efi.h>

// --- Raw ACPI structures --------------------------------------------------
//
// These structures and the table-walking API are arch-neutral. Decoding the
// per-arch entries inside the MADT (Local APIC on x86_64, GICC on aarch64,
// etc.) is the job of arch-specific code in archsrc/.

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

// Multiple APIC Description Table ("APIC" signature). On aarch64 this same
// table holds GICC/GICD/GICR/GICITS entries; the per-entry decoders are
// arch-specific.
struct acpi_madt {
  struct acpi_sdt_header header;
  uint32_t local_apic_address; // x86 only; ignored on other archs
  uint32_t flags;              // bit 0 = PCAT_COMPAT (legacy 8259 PIC present)
  // Followed by `length - sizeof(*this)` bytes of variable entries; each
  // entry begins with struct acpi_madt_entry_header.
} __attribute__((packed));

// Header on every MADT entry (ICS). Entry payload follows.
struct acpi_madt_entry_header {
  uint8_t type;
  uint8_t length;
} __attribute__((packed));

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

// Checked four-character SDT lookup shared by the architecture backends and
// the early platform registry. The returned table has a valid header length
// and checksum. `signature` need not be NUL terminated.
const struct acpi_sdt_header *acpi_find_table(const struct acpi_rsdp *rsdp,
                                              const char signature[4]);

#endif // acpi_h_INCLUDED
