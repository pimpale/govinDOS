#include "lapic.h"

#include <stdatomic.h>
#include <stdint.h>

#include "allocator.h"
#include "debug.h"
#include "madt_x86.h"
#include "paging.h"
#include "panic.h"
#include "serial.h"
#include "stdlib/stdio.h"

// LAPIC register offsets (xAPIC MMIO).
#define LAPIC_REG_ID 0x020 // bits 24..31 = APIC ID
#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_SVR 0x0F0 // spurious interrupt vector
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_ICR_LOW 0x300
#define LAPIC_REG_ICR_HIGH 0x310
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_ICR 0x380 // initial count (write starts countdown)
#define LAPIC_REG_TIMER_CCR 0x390 // current count (read-only)
#define LAPIC_REG_TIMER_DCR 0x3E0 // divide configuration

// LVT bit 16 masks delivery (the counter still counts). Timer mode lives
// in LVT bits 17..18; 00 = one-shot, the only mode used here.
#define LAPIC_LVT_MASKED (1u << 16)
// Divide-by-1 encoding for the DCR (bits 0, 1 and 3 set).
#define LAPIC_TIMER_DIV_1 0xBu

// SVR bit 8 = APIC software enable. Must be set for the LAPIC to deliver
// any interrupts (including IPIs). Spurious vector in bits 0..7 — pick
// 0xFF, an unused vector; the default ISR will print and continue if it
// ever fires.
#define LAPIC_SVR_ENABLE (1u << 8)
#define LAPIC_SPURIOUS_VECTOR 0xFFu

// ICR low fields. Vector goes in bits 0..7.
#define ICR_DELIVERY_INIT (5u << 8)
#define ICR_DELIVERY_SIPI (6u << 8)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_TRIGGER_LEVEL (1u << 15)
#define ICR_DELIVERY_PENDING (1u << 12)

static volatile uint32_t *g_lapic = nullptr;

static inline uint32_t lapic_read(uint32_t off) { return g_lapic[off / 4]; }

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

uint64_t x86_lapic_address(const struct acpi_madt *madt) {
  if (madt == nullptr) {
    fatal("provided invalid madt");
  }

  uint64_t addr = madt->local_apic_address;

  const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
  const uint8_t *end = (const uint8_t *)madt + madt->header.length;

  while (p + sizeof(struct acpi_madt_entry_header) <= end) {
    const struct acpi_madt_entry_header *eh =
        (const struct acpi_madt_entry_header *)p;
    if (eh->length < sizeof(*eh) || p + eh->length > end) {
      break;
    }
    if (eh->type == ACPI_MADT_LAPIC_ADDRESS_OVERRIDE) {
      const struct acpi_madt_lapic_address_override *ovr =
          (const struct acpi_madt_lapic_address_override *)p;
      addr = ovr->local_apic_address;
    }
    p += eh->length;
  }
  return addr;
}

void x86_lapic_init(const struct acpi_madt *madt) {
  uint64_t lapic_phys_base = x86_lapic_address(madt);

  if (lapic_phys_base == 0) {
    fatal("lapic: MADT reported no LAPIC base\n");
  }
  // UEFI identity-maps physical memory, so the physical base is also our
  // virtual address.
  g_lapic = (volatile uint32_t *)(uintptr_t)lapic_phys_base;
  printf("lapic: base=%016llX id=%u\n", lapic_phys_base,
         (uint32_t)x86_lapic_id());
}

uint8_t x86_lapic_id(void) { return (uint8_t)(lapic_read(LAPIC_REG_ID) >> 24); }

void x86_lapic_enable(void) {
  lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
  // Firmware may hand off in virtual-wire mode (LINT0 = ExtINT), which
  // would let the legacy PIC's INTR line inject vectors behind the
  // IOAPIC's back. Symmetric-I/O only: mask LINT0 on every CPU. LINT1
  // stays firmware-configured — it is conventionally the NMI wire.
  lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
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

void x86_lapic_eoi(void) { lapic_write(LAPIC_REG_EOI, 0); }

// ----- LAPIC timer -----------------------------------------------------------

// APIC-timer ticks per millisecond, measured once on the BSP. The timer
// clocks off the bus clock, which is shared by every CPU, so a single
// global measurement serves all of them.
static uint64_t g_lapic_timer_ticks_per_ms = 0;
static uint64_t g_tsc_hz = 0;
static uint64_t g_tsc_base = 0;
static _Atomic uint64_t g_monotonic_last_ns = 0;

static uint64_t read_tsc(void) {
  uint32_t lo;
  uint32_t hi;
  // LFENCE orders the sample after earlier loads. The kernel never migrates
  // while IRQs are disabled; modern x86 systems expose a synchronized,
  // invariant TSC across the CPUs GovindOS brings online.
  __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}

// PIT bits, used only during calibration (the PIT is otherwise unused —
// its IRQ is never routed; everything below is polled).
#define PIT_HZ 1193182u
#define PIT_PORT_CH2_DATA 0x42
#define PIT_PORT_CMD 0x43
#define PIT_PORT_NMI_STS 0x61 // ch2 gate (bit 0), speaker (bit 1), out (bit 5)

void x86_lapic_timer_calibrate(void) {
  // Run the LAPIC counter against a 10 ms one-shot on PIT channel 2.
  // Channel 2 is the only PIT channel whose gate and output are visible
  // through port 0x61, so it can be polled with no interrupt wiring. In
  // mode 0 the output sits low from the moment the count is loaded and
  // goes high at terminal count. Bit 1 stays low: speaker disconnected.
  const uint16_t pit_count = (uint16_t)(PIT_HZ / 100); // 10 ms
  uint8_t gate = inb(PIT_PORT_NMI_STS) & ~0x03u;
  outb(PIT_PORT_NMI_STS, gate); // gate low: loaded count holds
  outb(PIT_PORT_CMD, 0xB0);     // ch2, lobyte/hibyte, mode 0, binary
  outb(PIT_PORT_CH2_DATA, (uint8_t)pit_count);
  outb(PIT_PORT_CH2_DATA, (uint8_t)(pit_count >> 8));

  lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_1);
  // Masked: we poll the count, no interrupt wanted.
  lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);

  uint64_t tsc_start = read_tsc();
  outb(PIT_PORT_NMI_STS, gate | 0x01);           // gate high: PIT counts
  lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFFu); // LAPIC counts
  while ((inb(PIT_PORT_NMI_STS) & 0x20) == 0) {
    __asm__ volatile("pause");
  }
  uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CCR);
  uint64_t tsc_end = read_tsc();
  lapic_write(LAPIC_REG_TIMER_ICR, 0); // stop
  outb(PIT_PORT_NMI_STS, gate);        // gate back low

  g_lapic_timer_ticks_per_ms = (0xFFFFFFFFu - (uint64_t)remaining) / 10;
  uint64_t tsc_delta = tsc_end - tsc_start;
  g_tsc_hz = tsc_delta * PIT_HZ / pit_count;
  g_tsc_base = tsc_end;
  asserts(g_lapic_timer_ticks_per_ms > 0, "lapic: timer calibration failed");
  asserts(g_tsc_hz > 0, "clock: TSC calibration failed");
  printf("lapic: timer calibrated, %llu ticks/ms; tsc=%llu Hz\n",
         g_lapic_timer_ticks_per_ms, g_tsc_hz);
}

uint64_t x86_monotonic_ns(void) {
  asserts(g_tsc_hz != 0, "clock: used before calibration");
  uint64_t cycles = read_tsc() - g_tsc_base;
  // Split quotient/remainder so a long uptime cannot overflow cycles*1e9.
  uint64_t ns = (cycles / g_tsc_hz) * 1000000000ull +
                (cycles % g_tsc_hz) * 1000000000ull / g_tsc_hz;

  // Clamp across CPU migration/TSC skew. This is a monotonic clock, not a
  // uniqueness counter, so equal successive samples are valid.
  uint64_t seen = atomic_load_explicit(&g_monotonic_last_ns,
                                       memory_order_relaxed);
  while (seen < ns &&
         !atomic_compare_exchange_weak_explicit(
             &g_monotonic_last_ns, &seen, ns, memory_order_relaxed,
             memory_order_relaxed)) {
  }
  return seen > ns ? seen : ns;
}

void x86_lapic_timer_arm_oneshot(uint8_t vector, uint64_t us) {
  asserts(g_lapic_timer_ticks_per_ms > 0, "lapic: timer not calibrated");
  uint64_t max_us = UINT32_MAX * 1000ull / g_lapic_timer_ticks_per_ms;
  uint64_t ticks = us >= max_us
                       ? UINT32_MAX
                       : g_lapic_timer_ticks_per_ms * us / 1000;
  if (ticks == 0) {
    ticks = 1;
  }
  if (ticks > 0xFFFFFFFFu) {
    ticks = 0xFFFFFFFFu;
  }
  lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_1);
  lapic_write(LAPIC_REG_LVT_TIMER, vector); // one-shot, unmasked
  // Writing the initial count (re)starts the countdown, replacing any
  // pending shot — the per-dispatch re-arm relies on exactly that.
  lapic_write(LAPIC_REG_TIMER_ICR, (uint32_t)ticks);
}

void x86_lapic_timer_stop(void) {
  // Initial count 0 stops the countdown. Masking the LVT on top guards
  // the window where the count reached zero but delivery hasn't happened;
  // a shot already accepted into the IRR still lands and must be treated
  // as spurious by its handler.
  lapic_write(LAPIC_REG_TIMER_ICR, 0);
  lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
}
