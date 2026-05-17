#include "lapic.h"

#include <stdint.h>

#include "allocator.h"
#include "debug.h"
#include "panic.h"
#include "paging.h"
#include "serial.h"
#include "stdlib/stdio.h"

// LAPIC register offsets (xAPIC MMIO).
#define LAPIC_REG_ID         0x020   // bits 24..31 = APIC ID
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310

// ICR low fields. Vector goes in bits 0..7.
#define ICR_DELIVERY_INIT    (5u << 8)
#define ICR_DELIVERY_SIPI    (6u << 8)
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_TRIGGER_LEVEL    (1u << 15)
#define ICR_DELIVERY_PENDING (1u << 12)

static volatile uint32_t *g_lapic = nullptr;

static inline uint32_t lapic_read(uint32_t off) {
  return g_lapic[off / 4];
}

static inline void lapic_write(uint32_t off, uint32_t val) {
  g_lapic[off / 4] = val;
}

// Crude microsecond-scale delay. Reading port 0x80 (the BIOS POST diagnostic
// port) is the traditional way to burn ~1 us per access without a calibrated
// timer. Good enough for the INIT/SIPI handshake; replace once we have HPET.
static void io_delay_us(uint64_t us) {
  for (uint64_t i = 0; i < us; i++) {
    outb(0x80, 0);
  }
}

void x86_lapic_init(uint64_t lapic_phys_base) {
  if (lapic_phys_base == 0) {
    fatal("lapic: MADT reported no LAPIC base\n");
  }
  // UEFI identity-maps physical memory, so the physical base is also our
  // virtual address.
  g_lapic = (volatile uint32_t *)(uintptr_t)lapic_phys_base;
  printf("lapic: base=%016llX id=%u\n", lapic_phys_base,
         (uint32_t)x86_lapic_id());
}

uint8_t x86_lapic_id(void) {
  return (uint8_t)(lapic_read(LAPIC_REG_ID) >> 24);
}

// Spin until the previous IPI has left the LAPIC.
static void wait_for_idle(void) {
  while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
    // pause hint; compiler emits a rep nop
    __asm__ volatile("pause");
  }
}

// Writing ICR_LOW commits the IPI; ICR_HIGH must be set first because the
// hardware latches both halves on the low write.
static void send_ipi(uint8_t apic_id, uint32_t icr_low) {
  lapic_write(LAPIC_REG_ICR_HIGH, ((uint32_t)apic_id) << 24);
  lapic_write(LAPIC_REG_ICR_LOW, icr_low);
  wait_for_idle();
}

void x86_lapic_send_init(uint8_t apic_id) {
  // Assert INIT, then deassert. Modern CPUs only require the assert, but the
  // level-triggered deassert is documented and harmless.
  send_ipi(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
  io_delay_us(200);
  send_ipi(apic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL);
}

void x86_lapic_send_sipi(uint8_t apic_id, uint8_t vector) {
  send_ipi(apic_id, ICR_DELIVERY_SIPI | ICR_LEVEL_ASSERT | (uint32_t)vector);
}

void x86_lapic_send_fixed(uint8_t apic_id, uint8_t vector) {
  // Delivery mode 0 (fixed) and trigger mode 0 (edge) are encoded as zero
  // bits; only level=assert and the vector itself need to be set.
  send_ipi(apic_id, ICR_LEVEL_ASSERT | (uint32_t)vector);
}

void x86_lapic_eoi(void) {
  lapic_write(LAPIC_REG_EOI, 0);
}
