#include "smp.h"

#include <stdint.h>

#include "debug.h"
#include "lapic.h"
#include "panic.h"
#include "serial.h"
#include "stdlib/stdio.h"
#include "stdlib/string.h"

// Physical address where the AP starts executing on SIPI. Must be page
// aligned and below 1 MiB so it fits in a SIPI vector (vector*0x1000).
#define AP_TRAMPOLINE_BASE 0x8000
#define AP_TRAMPOLINE_PAGES 1
#define AP_SIPI_VECTOR (AP_TRAMPOLINE_BASE >> 12)

// Layout shared with ap_trampoline.asm — keep in sync.
#define AP_OFF_CR3   0x08
#define AP_OFF_ENTRY 0x10
#define AP_OFF_STACK 0x18
#define AP_OFF_ALIVE 0x20

extern uint8_t ap_trampoline_blob[];
extern uint8_t ap_trampoline_blob_end[];

static bool g_trampoline_installed = false;

static inline uint64_t read_cr3(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  return cr3;
}

static inline void io_delay_us(uint64_t us) {
  for (uint64_t i = 0; i < us; i++) {
    outb(0x80, 0);
  }
}

static uint64_t *trampoline_field(uint64_t offset) {
  return (uint64_t *)(uintptr_t)(AP_TRAMPOLINE_BASE + offset);
}

void cpu_signal_alive(void) {
  *(volatile uint64_t *)trampoline_field(AP_OFF_ALIVE) = 1;
}

static void install_trampoline(void) {
  uint64_t size = (uint64_t)(ap_trampoline_blob_end - ap_trampoline_blob);
  if (size > AP_TRAMPOLINE_PAGES * 4096) {
    fatal("smp: trampoline blob does not fit in reserved page\n");
  }
  memcpy((void *)(uintptr_t)AP_TRAMPOLINE_BASE, ap_trampoline_blob, size);
  g_trampoline_installed = true;
}

void cpu_start(uint64_t hw_id, void (*entry)(void), void *stack) {
  if (hw_id > 0xFF) {
    printf("smp: hw_id %llu out of xAPIC range, skipping\n", hw_id);
    return;
  }
  uint8_t apic_id = (uint8_t)hw_id;

  if (!g_trampoline_installed) {
    install_trampoline();
  }

  uint64_t cr3 = read_cr3();
  if ((cr3 >> 32) != 0) {
    // The trampoline loads CR3 in 32-bit protected mode (pre-paging), so the
    // PML4 must live below 4 GiB. UEFI normally places it there.
    fatal("smp: PML4 above 4 GiB, AP cannot enter long mode\n");
  }

  *trampoline_field(AP_OFF_CR3)   = cr3;
  *trampoline_field(AP_OFF_ENTRY) = (uint64_t)(uintptr_t)entry;
  *trampoline_field(AP_OFF_STACK) = (uint64_t)(uintptr_t)stack;
  *trampoline_field(AP_OFF_ALIVE) = 0;

  // INIT-SIPI-SIPI. Intel MP spec recommends two SIPIs; the first wakes the
  // AP, the second is ignored if the AP is already running.
  x86_lapic_send_init(apic_id);
  io_delay_us(10000);  // ~10 ms
  x86_lapic_send_sipi(apic_id, AP_SIPI_VECTOR);
  io_delay_us(200);
  x86_lapic_send_sipi(apic_id, AP_SIPI_VECTOR);

  // Wait for the AP to flip the alive flag. ~1 s timeout.
  volatile uint64_t *alive = trampoline_field(AP_OFF_ALIVE);
  for (uint64_t i = 0; i < 1000000; i++) {
    if (*alive) {
      return;
    }
    io_delay_us(1);
  }
  printf("smp: AP %u did not come up\n", (uint32_t)apic_id);
}
