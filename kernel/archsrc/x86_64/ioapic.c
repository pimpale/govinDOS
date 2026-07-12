#include "ioapic.h"

#include <stddef.h>
#include <stdint.h>

#include "debug.h"
#include "madt_x86.h"
#include "serial.h"
#include "spinlock.h"
#include "stdlib/stdio.h"

#define IOAPIC_MAX_CONTROLLERS 8
#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_REDIR 0x10
#define IOAPIC_RTE_MASK (1ull << 16)
#define IOAPIC_RTE_ACTIVE_LOW (1ull << 13)
#define IOAPIC_RTE_LEVEL (1ull << 15)

struct ioapic {
  volatile uint32_t *base;
  uint32_t gsi_base;
  uint32_t ninputs;
  struct spinlock lock;
};

struct source_override {
  bool present;
  bool active_low;
  enum irq_trigger_mode mode;
};

static struct ioapic g_ioapics[IOAPIC_MAX_CONTROLLERS];
static uint32_t g_nioapics;
static struct source_override g_isa_override[16];
static uint32_t g_isa_gsi[16];

static uint32_t reg_read_locked(struct ioapic *io, uint8_t reg) {
  io->base[0] = reg;
  return io->base[4];
}

static void reg_write_locked(struct ioapic *io, uint8_t reg, uint32_t value) {
  io->base[0] = reg;
  io->base[4] = value;
}

static void rte_write_locked(struct ioapic *io, uint32_t pin, uint64_t rte) {
  uint8_t reg = (uint8_t)(IOAPIC_REG_REDIR + 2 * pin);
  // Keep the input masked while changing its destination/vector.
  reg_write_locked(io, reg, (uint32_t)rte | (uint32_t)IOAPIC_RTE_MASK);
  reg_write_locked(io, reg + 1, (uint32_t)(rte >> 32));
  reg_write_locked(io, reg, (uint32_t)rte);
}

// Mask-bit flips touch only the lo dword — one select, one read, one
// write, instead of the full RTE read-modify-write (10 MMIO accesses).
// This is the per-interrupt path for level-triggered lines: it runs
// inside irq_deliver/ack under the route lock with IRQs off.
static void rte_set_mask_locked(struct ioapic *io, uint32_t pin, bool mask) {
  io->base[0] = (uint8_t)(IOAPIC_REG_REDIR + 2 * pin);
  uint32_t lo = io->base[4];
  io->base[4] = mask ? (lo | (uint32_t)IOAPIC_RTE_MASK)
                     : (lo & ~(uint32_t)IOAPIC_RTE_MASK);
}

static struct ioapic *find_ioapic(uint32_t gsi, uint32_t *pin_out) {
  for (uint32_t i = 0; i < g_nioapics; i++) {
    struct ioapic *io = &g_ioapics[i];
    if (gsi >= io->gsi_base && gsi - io->gsi_base < io->ninputs) {
      *pin_out = gsi - io->gsi_base;
      return io;
    }
  }
  return nullptr;
}

// Remap-then-mask. Masking alone is not enough: a masked 8259 can still
// deliver a spurious IR7 (noise asserts INTR, nothing qualifies at INTA
// time), and on firmware defaults that arrives as vector 0x0F — a
// reserved exception — which would panic the kernel. Park both PICs at
// PIC_VECTOR_BASE first so any such ghost lands where interrupt_handler
// ignores it.
static void remap_and_mask_legacy_pic(void) {
  outb(0x20, 0x11); // ICW1: init, ICW4 follows
  outb(0xa0, 0x11);
  outb(0x21, PIC_VECTOR_BASE); // ICW2: vector bases
  outb(0xa1, PIC_VECTOR_BASE + 8);
  outb(0x21, 0x04); // ICW3: slave on IR2 / cascade id 2
  outb(0xa1, 0x02);
  outb(0x21, 0x01); // ICW4: 8086 mode
  outb(0xa1, 0x01);
  outb(0x21, 0xff); // mask everything
  outb(0xa1, 0xff);
}

void x86_ioapic_init(const struct acpi_madt *madt) {
  asserts(madt != nullptr, "ioapic: missing MADT");
  g_nioapics = 0;
  for (uint32_t i = 0; i < 16; i++) {
    g_isa_override[i] = (struct source_override){0};
    g_isa_gsi[i] = i;
  }

  madt_for_each(madt, eh) {
    if (eh->type == ACPI_MADT_IO_APIC &&
        eh->length >= sizeof(struct acpi_madt_io_apic)) {
      asserts(g_nioapics < IOAPIC_MAX_CONTROLLERS, "ioapic: too many units");
      const struct acpi_madt_io_apic *a =
          (const struct acpi_madt_io_apic *)eh;
      struct ioapic *io = &g_ioapics[g_nioapics++];
      io->base = (volatile uint32_t *)(uintptr_t)a->io_apic_address;
      io->gsi_base = a->global_system_interrupt_base;
      spinlock_init(&io->lock);
      spinlock_lock(&io->lock);
      io->ninputs = ((reg_read_locked(io, IOAPIC_REG_VER) >> 16) & 0xff) + 1;
      spinlock_unlock(&io->lock);
    } else if (eh->type == ACPI_MADT_INTERRUPT_SOURCE_OVERRIDE &&
               eh->length >= sizeof(struct acpi_madt_interrupt_override)) {
      const struct acpi_madt_interrupt_override *o =
          (const struct acpi_madt_interrupt_override *)eh;
      if (o->bus == 0 && o->source < 16) {
        uint16_t polarity = o->flags & 3;
        uint16_t trigger = (o->flags >> 2) & 3;
        g_isa_override[o->source] = (struct source_override){
            .present = true,
            .active_low = polarity == 3,
            .mode = trigger == 3 ? IRQ_TRIGGER_LEVEL : IRQ_TRIGGER_EDGE,
        };
        g_isa_gsi[o->source] = o->global_system_interrupt;
      }
    }
  }

  // Firmware may leave arbitrary boot routes installed. Silence all inputs;
  // claims install a fresh fixed-delivery route before unmasking.
  for (uint32_t i = 0; i < g_nioapics; i++) {
    struct ioapic *io = &g_ioapics[i];
    spinlock_lock(&io->lock);
    for (uint32_t pin = 0; pin < io->ninputs; pin++) {
      rte_set_mask_locked(io, pin, true);
    }
    spinlock_unlock(&io->lock);
    printf("ioapic: gsi=%u..%u inputs=%u\n", io->gsi_base,
           io->gsi_base + io->ninputs - 1, io->ninputs);
  }
  if (madt->flags & 1)
    remap_and_mask_legacy_pic();
}

// Trigger/polarity for a GSI: architectural defaults — ISA inputs are
// high/edge, and ACPI's PCI routing is level/low, so without an AML
// interpreter non-ISA GSIs use that — overridden by any matching ISA
// source override (ISO target GSIs live in g_isa_gsi to keep
// source_override compact).
static void gsi_resolve(uint32_t gsi, bool *low_out,
                        enum irq_trigger_mode *mode_out) {
  *low_out = gsi >= 16;
  *mode_out = gsi >= 16 ? IRQ_TRIGGER_LEVEL : IRQ_TRIGGER_EDGE;
  for (uint32_t irq = 0; irq < 16; irq++) {
    if (g_isa_override[irq].present && g_isa_gsi[irq] == gsi) {
      *low_out = g_isa_override[irq].active_low;
      *mode_out = g_isa_override[irq].mode;
    }
  }
}

bool x86_ioapic_gsi_info(uint32_t gsi, enum irq_trigger_mode *mode_out) {
  uint32_t pin;
  if (find_ioapic(gsi, &pin) == nullptr)
    return false;
  bool low;
  gsi_resolve(gsi, &low, mode_out);
  return true;
}

bool x86_ioapic_program(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id) {
  uint32_t pin;
  struct ioapic *io = find_ioapic(gsi, &pin);
  if (io == nullptr)
    return false;
  bool low;
  enum irq_trigger_mode mode;
  gsi_resolve(gsi, &low, &mode);
  uint64_t rte = vector | IOAPIC_RTE_MASK | ((uint64_t)dest_apic_id << 56);
  if (low)
    rte |= IOAPIC_RTE_ACTIVE_LOW;
  if (mode == IRQ_TRIGGER_LEVEL)
    rte |= IOAPIC_RTE_LEVEL;
  spinlock_lock(&io->lock);
  rte_write_locked(io, pin, rte);
  spinlock_unlock(&io->lock);
  return true;
}

void x86_ioapic_mask(uint32_t gsi) {
  uint32_t pin;
  struct ioapic *io = find_ioapic(gsi, &pin);
  if (io == nullptr)
    return;
  spinlock_lock(&io->lock);
  rte_set_mask_locked(io, pin, true);
  spinlock_unlock(&io->lock);
}

void x86_ioapic_unmask(uint32_t gsi) {
  uint32_t pin;
  struct ioapic *io = find_ioapic(gsi, &pin);
  if (io == nullptr)
    return;
  spinlock_lock(&io->lock);
  rte_set_mask_locked(io, pin, false);
  spinlock_unlock(&io->lock);
}
