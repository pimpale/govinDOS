#ifndef irq_h_INCLUDED
#define irq_h_INCLUDED

// Per-CPU IRQ-disable nesting counter. Modeled on Linux's preempt_count:
// the depth lives on each CPU's struct cpu_state and is mutated only by
// code running on that CPU with IRQs already off, so it needs no atomics.
//
// Invariants:
//   depth == 0  <=>  IRQs should be enabled in hardware
//   depth >  0  <=>  IRQs are masked in hardware
//
// irq_disable / irq_enable balance each other; they may nest arbitrarily.
// Only the outermost pair touches IF (cli on 0->1, sti on 1->0).
void irq_disable(void);
void irq_enable(void);

// For interrupt-handler entry/exit ONLY. The CPU clears IF on interrupt
// gate entry and restores it from the iretq frame on exit, so the handler
// must not touch IF itself — but spinlocks inside the handler call
// irq_disable/enable, and without a depth bump the inner unlock would
// drop depth to 0 and sti mid-handler. irq_enter bumps depth without cli;
// irq_exit decrements without sti.
void irq_enter(void);
void irq_exit(void);

#endif
