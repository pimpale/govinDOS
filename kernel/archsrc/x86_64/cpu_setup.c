#include "cpu_setup.h"

#include "gdt.h"
#include "interrupts.h"
#include "paging.h"

// Identity-map the first 512 GiB of physical space in the kernel AS.
// 512 × 1 GiB huge pages costs one PML4 entry pointing at one PDPT;
// the implementation in paging.c picks huge pages automatically when
// the range is large and aligned.
#define KERNEL_IDENTITY_LIMIT (512ull * 1024 * 1024 * 1024)

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

void paging_init_shared(void) {
    // Bootstrap the kernel AS's root page table. After this, as_kernel()
    // returns a usable AS that the rest of the API can manipulate.
    as_init_kernel();

    // Identity baseline. RAM, kernel image, AP trampoline, and (for now)
    // all common MMIO are covered with WB caching and kernel-only access.
    // Cache-type overrides for specific MMIO ranges are out of scope for
    // the initial bring-up; add them with as_protect_range later.
    as_map_range(as_kernel(), 0, KERNEL_IDENTITY_LIMIT, PAGE_R | PAGE_W);

    // Freeze. Any further mutation of the kernel AS is now a programming
    // error and will assert.
    as_seal(as_kernel());
}



void cpu_setup_bsp(void) {
    // Build data structures that every CPU will share:
    //   - the kernel page tables (paging_init_shared)
    //   - the IDT contents (bsp_prepare_interrupt_table)
    // Neither is installed on this CPU yet; cpu_setup() does that below.
    paging_init_shared();
    interrupts_fill_idt();
}

void cpu_setup(void) {
    // 1. Construct and load this CPU's GDT (also installs the TSS so that
    //    IST-using exceptions and any future ring transitions have a stack
    //    to switch to).
    cpu_install_gdt_tss();

    // 2. Load the IDT. Depends on the GDT already being installed because
    //    the IDT gates reference kernel CS by selector value.
    interrupts_load_idt();

    // 3. Switch onto the kernel page tables and enable NX. Done last so
    //    that if the new tables are wrong, the resulting #PF lands on the
    //    handlers we just installed rather than on whatever firmware left
    //    behind.
    enable_nxe();
    as_switch(as_kernel());
}
