#ifndef gdt_h_INCLUDED
#define gdt_h_INCLUDED

#include <stdint.h>

// Per-CPU GDT + TSS setup for x86_64 long mode.
//
// The legacy bootstrap GDT in gdt.asm is a single shared table used only
// before this code runs. As soon as a CPU is in the kernel C environment,
// it should call cpu_install_gdt_tss() to switch to its own per-CPU GDT
// (which carries its own TSS descriptor) and load its TR.
//
// Why per-CPU: the TSS holds RSP0 and the IST stack pointers, which the
// CPU automatically loads into RSP on ring transitions and on exceptions
// configured with an IST index. Two CPUs sharing one TSS would race on
// those stacks.

// Selector values used by the per-CPU GDT installed here. They are
// fixed (not per-CPU) so that the IDT and any asm stubs can reference
// them by constant. Layout is chosen to be compatible with syscall/sysret
// (STAR MSR), so adding userspace later does not require reshuffling.
//
//   0x00: null
//   0x08: kernel code  (CS, DPL=0, L=1)
//   0x10: kernel data  (SS/DS, DPL=0)
//   0x18: user code 32 (placeholder; required by STAR layout)
//   0x20: user data    (SS/DS, DPL=3)
//   0x28: user code 64 (CS, DPL=3, L=1)
//   0x30..0x38: TSS descriptor (16 bytes, two slots)
#define GDT_SEL_KERNEL_CS  0x08
#define GDT_SEL_KERNEL_DS  0x10
#define GDT_SEL_USER_CS32  (0x18 | 3)
#define GDT_SEL_USER_DS    (0x20 | 3)
#define GDT_SEL_USER_CS64  (0x28 | 3)
#define GDT_SEL_TSS        0x30

// IST slot assignments (1-based, as the hardware encodes them in IDT gates).
// Slot 0 means "no IST" — the CPU uses RSP0 (on ring change) or the current
// stack (no ring change).
#define IST_DOUBLE_FAULT   1
#define IST_NMI            2
#define IST_PAGE_FAULT     3
#define IST_MACHINE_CHECK  4

// One-time per-CPU bring-up. Allocates this CPU's GDT and TSS, populates
// the descriptors using the caller-provided stacks, and installs them via
// lgdt + segment reloads + ltr. Call once on each CPU (BSP and every AP)
// from its C entry point, before enabling interrupts.
//
// Each stack pointer is the *top* of an already-allocated kernel stack
// whose bottom page has been mapped absent as a guard. The caller owns
// these allocations; this function only records them in the TSS.
//
// On return, this CPU is running with its own GDT loaded, segment registers
// pointing at the new kernel selectors, and TR pointing at the new TSS.
void cpu_install_gdt_tss(
    void *rsp0_stack_top,
    void *ist_double_fault_stack_top,
    void *ist_nmi_stack_top,
    void *ist_page_fault_stack_top,
    void *ist_machine_check_stack_top);

#endif // gdt_h_INCLUDED
