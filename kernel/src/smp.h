#ifndef smp_h_INCLUDED
#define smp_h_INCLUDED

#include <stdint.h>

// Wake a secondary CPU. On return the AP has been told to start; it is not
// guaranteed to have reached `entry` yet (callers should wait on a shared
// "I'm alive" flag set by the AP).
//
//   hw_id  - identifier from struct cpu.hw_id (LAPIC ID on x86, MPIDR on arm).
//   entry  - address the AP should begin executing at, in 64-bit kernel mode
//            with paging/MMU configured the same way as the BSP.
//   stack  - top of the per-AP stack (stack grows down from here).
//
// Implementation lives in archsrc/<arch>/smp.c:
//   - x86_64:  INIT IPI + SIPI through the LAPIC, via a low-memory trampoline.
//   - aarch64: PSCI CPU_ON (HVC/SMC) — AP lands at `entry` already in EL1.
void cpu_start(uint64_t hw_id, void (*entry)(void), void *stack);

#endif // smp_h_INCLUDED
