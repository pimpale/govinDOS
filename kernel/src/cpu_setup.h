#ifndef cpu_setup_h_INCLUDE
#define cpu_setup_h_INCLUDE

#include "acpi.h"

// this function should be called once. It populates various shared data structures (eg, the interrupt table, paging tables) which are shared between all cpus.
void cpu_setup_bsp(const struct acpi_rsdp *rsdp);

// this function must be called on all cpus (including the BSP) to initialize: tss, gdt, interrupts, paging.
// before calling this function you must:
// 1. set up the global allocator
// 2. have called cpu_setup_bsp() for initializing shared data structures ONLY on the BSP processor
// 3. allocated this CPU's kernel + IST stacks (with their bottom page mapped
//    absent as a guard) and obtained their tops. Each stack is single-owner
//    and must outlive this CPU.
void cpu_setup(
    void *rsp0_stack_top,
    void *ist_double_fault_stack_top,
    void *ist_nmi_stack_top,
    void *ist_page_fault_stack_top,
    void *ist_machine_check_stack_top);

// it is heavily architecture dependent, so the implementation is in archsrc/<arch>/cpu_setup.c

#endif // cpu_setup_h_INCLUDE