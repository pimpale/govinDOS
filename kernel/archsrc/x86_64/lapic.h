#ifndef lapic_h_INCLUDED
#define lapic_h_INCLUDED

#include <stdint.h>
#include "acpi.h"

struct address_space;

// Returns the effective LAPIC MMIO address: starts at madt->local_apic_address
// and is replaced by the first Type-5 override if present. Returns 0 if `madt`
// is null.
uint64_t x86_lapic_address(const struct acpi_madt *madt);

// xAPIC handler. We assume the LAPIC MMIO base is identity-mapped (UEFI's
// page tables cover the typical 0xFEE00000 region during early boot; the
// kernel address space marks the page uncached before switching to it).
// x2APIC is not supported here — see enumerate_cpus.c which already excludes
// APIC IDs > 254.

// Records the MMIO base of the local APIC. Call once on the BSP after
// exit_boot_services
void x86_lapic_init(const struct acpi_madt *madt);

// LAPIC ID of the currently executing CPU. Reads bits 24..31 of the ID
// register (offset 0x20). Valid only after x86_lapic_init.
uint8_t x86_lapic_id(void);

// Set this CPU's local-APIC Spurious Interrupt Vector Register so its
// software-enable bit is on. Without this, the LAPIC silently drops
// every IPI directed at this CPU. Must run on every CPU during setup,
// after x86_lapic_init has recorded the MMIO base.
void x86_lapic_enable(void);

// Send an INIT IPI (level-triggered assert + edge deassert) to `apic_id`.
// Blocks until the LAPIC reports the IPI as delivered.
void x86_lapic_send_init(uint8_t apic_id);

// Send a Startup IPI (SIPI). `vector` is the page number of the AP's
// real-mode entry: physical entry address = vector << 12. Valid range is
// 0x00..0xFF, but the AP is started at vector*0x1000 so practical range is
// constrained by the bootloader's low-memory reservation.
void x86_lapic_send_sipi(uint8_t apic_id, uint8_t vector);

// Send a fixed-priority, edge-triggered IPI to `apic_id` with the given
// IDT vector. The receiver runs its installed ISR for that vector and
// must EOI before returning. Used for cross-CPU signalling (e.g. TLB
// shootdown).
void x86_lapic_send_fixed(uint8_t apic_id, uint8_t vector);

// Acknowledge the in-service interrupt on this CPU's local APIC. Must be
// called at the end of any LAPIC-delivered interrupt handler or the LAPIC
// will refuse further same- or lower-priority interrupts.
void x86_lapic_eoi(void);

#endif // lapic_h_INCLUDED
