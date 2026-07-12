#ifndef ioapic_h_INCLUDED
#define ioapic_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "acpi.h"

enum irq_trigger_mode {
  IRQ_TRIGGER_EDGE,
  IRQ_TRIGGER_LEVEL,
};

// Where the legacy 8259 PICs are parked (remap-then-mask): a masked 8259
// can still emit a spurious IR7, so its vectors must land in a range the
// interrupt handler tolerates, not on the CPU exceptions the firmware
// default (0x08/0x70) collides with. 0x20..0x2F sits between the
// exception block and VECTOR_DEVICE_BASE.
#define PIC_VECTOR_BASE 0x20
#define PIC_VECTOR_END 0x2F

// Discover IOAPICs and source overrides and leave every input masked.
void x86_ioapic_init(const struct acpi_madt *madt);

// GSI metadata and RTE operations. All functions are bounded; the driver
// serializes each IOREGSEL/IOWIN window internally.
bool x86_ioapic_gsi_info(uint32_t gsi, enum irq_trigger_mode *mode_out);
bool x86_ioapic_program(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id);
void x86_ioapic_mask(uint32_t gsi);
void x86_ioapic_unmask(uint32_t gsi);

#endif // ioapic_h_INCLUDED
