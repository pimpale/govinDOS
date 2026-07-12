#ifndef irq_scheme_h_INCLUDED
#define irq_scheme_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "acpi.h"

#define VECTOR_DEVICE_BASE 0x30
#define VECTOR_DEVICE_END 0xEF

// BSP-only hardware/route initialization, after MADT MMIO pages are mapped.
void irq_scheme_init(const struct acpi_madt *madt);

static inline bool irq_is_device_vector(uint64_t vector) {
  return vector >= VECTOR_DEVICE_BASE && vector <= VECTOR_DEVICE_END;
}

// Interrupt-context delivery. Always EOIs the local APIC.
void irq_deliver(uint8_t vector);

#endif // irq_scheme_h_INCLUDED
