#include "irq.h"

#include "cpu_state.h"
#include "debug.h"

// cli must happen *before* we look up the per-CPU slot: cpu_state_whoami
// reads the CPU id from hardware, and we need IRQs masked to guarantee we
// don't migrate (or get re-entered by a handler) between the lookup and
// the depth bump.
void irq_disable(void) {
  __asm__ volatile("cli" ::: "memory");
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  cs->irq_depth++;
}

void irq_enable(void) {
  // Safe to read per-CPU slot without first disabling IRQs: depth > 0
  // here implies they're already off.
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  asserts(cs->irq_depth > 0, "irq_enable: depth underflow");
  if (--cs->irq_depth == 0) {
    __asm__ volatile("sti" ::: "memory");
  }
}

void irq_enter(void) {
  // Called from interrupt entry. Hardware has already cleared IF, so
  // we're pinned and can touch per-CPU state without our own cli.
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  cs->irq_depth++;
}

void irq_exit(void) {
  struct cpu_state *cs = &g_cpu_state_table[cpu_state_whoami()];
  asserts(cs->irq_depth > 0, "irq_exit: depth underflow");
  cs->irq_depth--;
  // Deliberately no sti: iretq restores IF from the saved frame.
}
