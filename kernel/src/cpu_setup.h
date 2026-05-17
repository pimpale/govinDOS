#ifndef cpu_setup_h_INCLUDE
#define cpu_setup_h_INCLUDE


// this function should be called once. It populates various shared data structures (eg, the interrupt table, paging tables) which are shared between all cpus.
void cpu_setup_bsp();

// this function must be called on all cpus (including the BSP) to initialize: tss, gdt, interrupts, paging. 
// before calling this function you must:
// 1. set up the global allocator
// 2. have called cpu_setup_bsp() for initializing shared data structures ONLY on the BSP processor
void cpu_setup();

// it is heavily architecture dependent, so the implementation is in archsrc/<arch>/cpu_setup.c

#endif // cpu_setup_h_INCLUDE