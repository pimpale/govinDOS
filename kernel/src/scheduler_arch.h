#ifndef scheduler_arch_h_INCLUDED
#define scheduler_arch_h_INCLUDED

#include <stdint.h>

// Puts the current CPU into a low-power wait until the next interrupt.
// Must be entered with IRQs enabled (otherwise nothing can wake us).
void scheduler_arch_idle(void);

// Kick the CPU identified by `hw_id` so its scheduler loop iterates.
// Used when we enqueue work onto a CPU that is parked in scheduler_arch_idle.
// Arch impl picks the mechanism (LAPIC IPI on x86, SGI on aarch64, etc.);
// the wake signal must be routed to a handler that does nothing more than
// EOI — the iretq alone is what bounces the target out of hlt/wfi.
void scheduler_arch_wake_cpu(uint64_t hw_id);

#endif // scheduler_arch_h_INCLUDED
