#include "cpu_setup.h"

#include <stdint.h>

#include "gdt.h"
#include "interrupts.h"
#include "lapic.h"
#include "paging.h"
#include "allocator.h"
#include "madt_x86.h"
#include "debug.h"

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
static void map_mmio_page(uint64_t phys) {
    if (phys == 0) return;
    uint64_t base = phys & ~((uint64_t)PAGE_SIZE - 1);
    as_flag(g_as_kernel, base, base + PAGE_SIZE, PAGE_R | PAGE_W | PAGE_UC);

    struct frame_info *fi = frame_for(base);
    if (fi != nullptr) {
        fi->kind = FRAME_MMIO;
    }
}

// Walk the MADT and mark every device MMIO region it advertises as UC in the
// kernel AS. Covers the LAPIC base (with any Type-5 override) and every
// IO APIC (Type 1). The default kernel AS identity-maps RAM as WB; remapping
// these specific 4 KiB frames as UC overrides that for device access.
static void map_acpi_mmio(const struct acpi_madt *madt) {
    map_mmio_page(x86_lapic_address(madt));

    const uint8_t *p   = (const uint8_t *)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;
    while (p + sizeof(struct acpi_madt_entry_header) <= end) {
        const struct acpi_madt_entry_header *eh =
            (const struct acpi_madt_entry_header *)p;
        if (eh->length < sizeof(*eh) || p + eh->length > end) break;
        if (eh->type == ACPI_MADT_IO_APIC) {
            const struct acpi_madt_io_apic *ioapic =
                (const struct acpi_madt_io_apic *)p;
            map_mmio_page(ioapic->io_apic_address);
        }
        p += eh->length;
    }
}

void cpu_setup_bsp(const struct acpi_rsdp *rsdp) {
    // get madt
    const struct acpi_madt* madt = acpi_find_madt(rsdp);
    asserts(madt != nullptr, "madt cannot be null");

    // kernel address space
    g_as_kernel = as_identity_mapping();

    // Override the identity mapping for every MMIO region the MADT
    // advertises so device accesses bypass the cache.
    map_acpi_mmio(madt);

    interrupts_fill_idt();
}

void cpu_setup(
    void *rsp0_stack_top,
    void *ist_double_fault_stack_top,
    void *ist_nmi_stack_top,
    void *ist_page_fault_stack_top,
    void *ist_machine_check_stack_top) {
    // 1. Construct and load this CPU's GDT (also installs the TSS so that
    //    IST-using exceptions and any future ring transitions have a stack
    //    to switch to).
    cpu_install_gdt_tss(rsp0_stack_top,
                        ist_double_fault_stack_top,
                        ist_nmi_stack_top,
                        ist_page_fault_stack_top,
                        ist_machine_check_stack_top);

    // 2. Load the IDT. Depends on the GDT already being installed because
    //    the IDT gates reference kernel CS by selector value.
    interrupts_load_idt();

    // 3. Switch onto the kernel page tables and enable NX. Done last so
    //    that if the new tables are wrong, the resulting #PF lands on the
    //    handlers we just installed rather than on whatever firmware left
    //    behind.
    enable_nxe();
    as_switch(g_as_kernel);
}
