#include "scheduler_arch.h"

#include "lapic.h"
#include "scheduler.h"

void scheduler_arch_idle(void) {
    __asm__ ("hlt");
}

// Cross-CPU wake via a fixed-delivery LAPIC IPI on VECTOR_RESCHED.
// The handler in interrupts.c just EOIs; the value of bouncing the
// target CPU is purely the iretq, which makes hlt return so the
// scheduler loop iterates and notices the newly-enqueued work.
void scheduler_arch_wake_cpu(uint64_t hw_id) {
    // xAPIC ICR encodes the destination in 8 bits. x2APIC would let us go
    // wider; callers above 0xFF won't be reachable on legacy APIC.
    x86_lapic_send_fixed((uint8_t)hw_id, VECTOR_RESCHED);
}
