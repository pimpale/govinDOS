#include "cpu_setup.h"

#include <stdint.h>

#include "debug.h"
#include "gdt.h"
#include "interrupts.h"
#include "lapic.h"
#include "madt_x86.h"
#include "irq_scheme.h"
#include "paging.h"
#include "stacks.h"
#include "stdlib/stdlib.h"

static uint64_t rdmsr64(uint32_t msr) {
  uint32_t eax, edx;
  __asm__ volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));
  return ((uint64_t)edx << 32) | eax;
}

static void wrmsr64(uint32_t msr, uint64_t val) {
  __asm__ volatile("wrmsr"
                   :
                   : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

#define IA32_EFER           0xC0000080u
#define IA32_STAR           0xC0000081u
#define IA32_LSTAR          0xC0000082u
#define IA32_FMASK          0xC0000084u
#define IA32_KERNEL_GS_BASE 0xC0000102u

static void enable_nxe(void) {
  // EFER.NXE (bit 11). Without this, any PTE with bit 63 set faults as
  // a reserved-bit violation rather than as #PF on instruction fetch.
  // UEFI usually sets this, but we own this MSR from here on.
  wrmsr64(IA32_EFER, rdmsr64(IA32_EFER) | (1u << 11));
}

static void enable_sse(void) {
  // FPU/SSE for userspace (the kernel itself is -mgeneral-regs-only and
  // never executes SIMD; fxsave64/fxrstor64 at thread park/resume are the
  // only FPU touches). CR0: MP set, EM/TS clear — a real FPU, and no
  // lazy-switch #NM traps, since the park/resume save is eager. CR4:
  // OSFXSR enables fxsave/fxrstor and unmasks SSE instructions;
  // OSXMMEXCPT makes an unmasked SSE exception #XM instead of #UD.
  // UEFI x64 already runs with SSE on, but we own these registers now.
  uint64_t cr0, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 |= 1u << 1;                       // MP
  cr0 &= ~((1u << 2) | (1u << 3));      // ~EM, ~TS
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1u << 9) | (1u << 10);        // OSFXSR, OSXMMEXCPT
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

// SYSCALL entry point, interrupts.asm.
extern void syscall_entry_stub(void);

// Per-CPU SYSCALL/SYSRET setup. The GDT layout was chosen for this from
// the start (gdt.h): SYSCALL loads CS/SS = STAR[47:32]+0/+8 = kernel
// CS/DS; SYSRET loads CS/SS = STAR[63:48]+16/+8 = user CS64/DS.
static void enable_syscall(struct cpu_state *cs) {
  wrmsr64(IA32_EFER, rdmsr64(IA32_EFER) | 1u); // EFER.SCE
  wrmsr64(IA32_STAR, ((uint64_t)GDT_SEL_USER_CS32 << 48) |
                         ((uint64_t)GDT_SEL_KERNEL_CS << 32));
  wrmsr64(IA32_LSTAR, (uint64_t)syscall_entry_stub);
  // Cleared in RFLAGS on entry: IF (the stub runs IRQs-off like an
  // interrupt gate), TF/AC/NT (no user-controlled trap/alignment state
  // in the kernel), DF (the C ABI assumes it clear).
  wrmsr64(IA32_FMASK, 0x100 | 0x200 | 0x400 | 0x4000 | 0x40000);
  // gs-relative anchor for the stub's manual stack switch. The user
  // GS base (plain IA32_GS_BASE) stays whatever ring 3 set; the stub
  // brackets its two gs loads in swapgs. (An NMI landing between them
  // would see the kernel GS base, but the NMI handler never touches gs
  // and is fatal anyway.)
  cs->syscall_anchor.kernel_rsp0_top = (uint64_t)cs->stacks.kernel_rsp0_top;
  wrmsr64(IA32_KERNEL_GS_BASE, (uint64_t)&cs->syscall_anchor);
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

  madt_for_each(madt, eh) {
    if (eh->type == ACPI_MADT_IO_APIC) {
      const struct acpi_madt_io_apic *ioapic =
          (const struct acpi_madt_io_apic *)eh;
      map_mmio_page(as, ioapic->io_apic_address);
    }
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

  // Calibrate the LAPIC timer (preemption quanta) before any AP can
  // reach its scheduler loop and arm it. Needs the BSP's LAPIC
  // software-enabled first; cpu_setup() re-enables it later along with
  // every other CPU's, which is idempotent.
  x86_lapic_enable();
  x86_lapic_timer_calibrate();

  // Discover and silence every IOAPIC input before userspace can claim one.
  // Device routes target this BSP in v1.
  irq_scheme_init(madt);
}

void cpu_setup() {
  cpu_state_table_require();
  // get the correct cpu entry:
  struct cpu_state *this_cpu_state = &g_cpu_state_table[cpu_state_whoami()];

  asserts(!this_cpu_state->called_cpu_setup, "can't call setup twice on this");
  this_cpu_state->called_cpu_setup = true;

  // 0. enable nxe (we don't use this yet but we will ig)
  enable_nxe();

  // 0.25. FPU/SSE control bits (user threads own the FPU; preemption
  //       means their SIMD state is live at any instruction boundary).
  enable_sse();

  // 0.5. SYSCALL/SYSRET (the only syscall path — there is no int gate).
  enable_syscall(this_cpu_state);

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
