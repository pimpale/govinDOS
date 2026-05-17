#ifndef madt_x86_h_INCLUDED
#define madt_x86_h_INCLUDED

#include <stdint.h>

#include "acpi.h"

// x86-only MADT entry types and payloads. aarch64 has its own (GICC, GICD, ...).

enum acpi_madt_entry_type_x86 {
  ACPI_MADT_LOCAL_APIC                = 0,
  ACPI_MADT_IO_APIC                   = 1,
  ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE = 2,
  ACPI_MADT_LAPIC_ADDRESS_OVERRIDE    = 5,
  ACPI_MADT_LOCAL_X2APIC              = 9,
};

// Type 0
struct acpi_madt_local_apic {
  struct acpi_madt_entry_header header;
  uint8_t  acpi_processor_id;
  uint8_t  apic_id;
  uint32_t flags;            // bit 0 = enabled, bit 1 = online_capable
} __attribute__((packed));

// Type 1
struct acpi_madt_io_apic {
  struct acpi_madt_entry_header header;
  uint8_t  io_apic_id;
  uint8_t  reserved;
  uint32_t io_apic_address;
  uint32_t global_system_interrupt_base;
} __attribute__((packed));

// Type 5 (overrides MADT.local_apic_address)
struct acpi_madt_lapic_address_override {
  struct acpi_madt_entry_header header;
  uint16_t reserved;
  uint64_t local_apic_address;
} __attribute__((packed));

// Returns the effective LAPIC MMIO address: starts at madt->local_apic_address
// and is replaced by the first Type-5 override if present. Returns 0 if `madt`
// is null.
uint64_t x86_lapic_address(const struct acpi_madt *madt);

#endif // madt_x86_h_INCLUDED
