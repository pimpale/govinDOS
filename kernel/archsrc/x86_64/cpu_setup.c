#include "cpu_setup.h"

#include <stdint.h>

#include "debug.h"
#include "gdt.h"
#include "interrupts.h"
#include "lapic.h"
#include "madt_x86.h"
#include "paging.h"
#include "stacks.h"
#include "stdlib/stdlib.h"

static void enable_nxe(void) {
  // EFER.NXE (bit 11). Without this, any PTE with bit 63 set faults as
  // a reserved-bit violation rather than as #PF on instruction fetch.
  // UEFI usually sets this, but we own this MSR from here on.
  const uint32_t IA32_EFER = 0xC0000080;
  uint32_t eax, edx;
  __asm__ volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(IA32_EFER));
  eax |= (1u << 11);
  __asm__ volatile("wrmsr" : : "a"(eax), "d"(edx), "c"(IA32_EFER));
}

// Mark the 4 KiB MMIO frame containing `phys` as R|W|UC in the kernel AS.
// MMIO registers must not be cached: stores need to reach the device in
// program order and reads must observe live device state.
static void map_mmio_page(struct address_space *as, uint64_t phys) {
  if (phys == 0)
    return;
  uint64_t base = phys & ~((uint64_t)PAGE_SIZE - 1);
  as_flag(as, base, base + PAGE_SIZE, PAGE_R | PAGE_W | PAGE_UC);
}

// Walk the MADT and mark every device MMIO region it advertises as UC in the
// kernel AS. Covers the LAPIC base (with any Type-5 override) and every
// IO APIC (Type 1). The default kernel AS identity-maps RAM as WB; remapping
// these specific 4 KiB frames as UC overrides that for device access.
static void map_acpi_mmio(struct address_space *as,
                          const struct acpi_madt *madt) {
  map_mmio_page(as, x86_lapic_address(madt));

  const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
  const uint8_t *end = (const uint8_t *)madt + madt->header.length;
  while (p + sizeof(struct acpi_madt_entry_header) <= end) {
    const struct acpi_madt_entry_header *eh =
        (const struct acpi_madt_entry_header *)p;
    if (eh->length < sizeof(*eh) || p + eh->length > end)
      break;
    if (eh->type == ACPI_MADT_IO_APIC) {
      const struct acpi_madt_io_apic *ioapic =
          (const struct acpi_madt_io_apic *)p;
      map_mmio_page(as, ioapic->io_apic_address);
    }
    p += eh->length;
  }
}

void cpu_setup_bsp(const struct acpi_rsdp *rsdp) {
  // get madt
  const struct acpi_madt *madt = acpi_find_madt(rsdp);
  asserts(madt != nullptr, "madt cannot be null");

  // kernel address space
  g_as_kernel = as_identity_mapping();

  // enable the lapic
  x86_lapic_init(madt);

  // Override the identity mapping for every MMIO region the MADT
  // advertises so device accesses bypass the cache.
  map_acpi_mmio(g_as_kernel, madt);

  // allocate per-cpu kernel stacks for all of the cpus
  for (size_t i = 0; i < g_cpu_state_table_len; i++) {
    // interrupt stacks
    g_cpu_state_table[i].stacks.kernel_rsp0_top =
        stacks_alloc_kernel(STACK_TYPE_KERNEL_INTERRUPT);
    g_cpu_state_table[i].stacks.ist_double_fault_top =
        stacks_alloc_kernel(STACK_TYPE_KERNEL_INTERRUPT);
    g_cpu_state_table[i].stacks.ist_machine_check_top =
        stacks_alloc_kernel(STACK_TYPE_KERNEL_INTERRUPT);
    g_cpu_state_table[i].stacks.ist_nmi_top =
        stacks_alloc_kernel(STACK_TYPE_KERNEL_INTERRUPT);
    g_cpu_state_table[i].stacks.ist_page_fault_top =
        stacks_alloc_kernel(STACK_TYPE_KERNEL_INTERRUPT);
    // bootstrap stack
    g_cpu_state_table[i].stacks.kernel_bootstrap_top =
        stacks_alloc_kernel(STACK_TYPE_KERNEL_BOOTSTRAP);
  }

  // prepare the idt vector to be loaded
  interrupts_fill_idt();
}

void cpu_setup() {
  cpu_state_table_require();
  // get the correct cpu entry:
  struct cpu_state *this_cpu_state = &g_cpu_state_table[cpu_state_whoami()];

  asserts(!this_cpu_state->called_cpu_setup, "can't call setup twice on this");
  this_cpu_state->called_cpu_setup = true;

  // 0. enable nxe (we don't use this yet but we will ig)
  enable_nxe();

  // 1. Construct and load this CPU's GDT (also installs the TSS so that
  //    IST-using exceptions and ring transitions have a stack to switch to).
  //    RSP0 is per-CPU and set once here; the TSS is not rewritten on
  //    context switch.
  cpu_install_gdt_tss(this_cpu_state->stacks.kernel_rsp0_top,
                      this_cpu_state->stacks.ist_double_fault_top,
                      this_cpu_state->stacks.ist_nmi_top,
                      this_cpu_state->stacks.ist_page_fault_top,
                      this_cpu_state->stacks.ist_machine_check_top);

  // 1.5. Record this CPU's identity in IA32_TSC_AUX so cpu_state_this()
  //      is O(1) (rdpid/rdtscp + table index). Position is arbitrary —
  //      the MSR is untouched by segment reloads and unreachable from
  //      ring 3 — but per-CPU bring-up is the natural home.
  cpu_percpu_install(this_cpu_state);

  // 2. Load the IDT. Depends on the GDT already being installed because
  //    the IDT gates reference kernel CS by selector value.
  interrupts_load_idt();

  // 3. Switch onto the kernel page tables
  as_switch(g_as_kernel);

  // 4. Enable this CPU's LAPIC (software-enable bit in SVR). Without
  //    this, the LAPIC drops every IPI directed at this CPU.
  x86_lapic_enable();
}
