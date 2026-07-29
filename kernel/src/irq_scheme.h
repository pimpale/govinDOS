#ifndef irq_scheme_h_INCLUDED
#define irq_scheme_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "acpi.h"

#define VECTOR_DEVICE_BASE 0x30
#define VECTOR_DEVICE_END 0xEF
#define VECTOR_IOMMU_FAULT 0xFA

// BSP-only hardware/route initialization, after MADT MMIO pages are mapped.
void irq_scheme_init(const struct acpi_madt *madt);

static inline bool irq_is_device_vector(uint64_t vector) {
  return vector >= VECTOR_DEVICE_BASE && vector <= VECTOR_DEVICE_END;
}

// Interrupt-context delivery. Always EOIs the local APIC.
void irq_deliver(uint8_t vector);

// Reap one unbound MSI grant targeting p. Called under g_umem before block
// teardown so a dead child cannot leave vector authority behind.
struct process;
struct irq_route;

// Capability parameter/liveness helpers. g_umem held by callers.
struct irq_route *irq_route_for_gsi_locked(uint64_t gsi);
uint32_t irq_route_id_locked(const struct irq_route *route);

#endif // irq_scheme_h_INCLUDED
