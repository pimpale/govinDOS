#ifndef cpu_data_h_INCLUDE
#define cpu_data_h_INCLUDE

#include <stdint.h>
#include "acpi.h"
#include "scheduler.h"

// opaque structs
struct address_space;
struct thread;

// Per-CPU kernel stacks. RSP0 (the stack the CPU loads on a ring-3 -> ring-0
// transition) is also per-CPU: a single stack shared by every syscall/
// interrupt-from-userspace on this CPU, so the TSS never has to be rewritten
// after install.
struct cpu_stacks {
  // Bootstrap stack used during cpu_setup. Once threading takes over, this
  // stack is abandoned in favor of the per-thread kernel stacks.
  void *kernel_bootstrap_top;
  // Per-cpu interrupt stacks (constant for the life of the cpu).
  void *kernel_rsp0_top;
  void *ist_double_fault_top;
  void *ist_nmi_top;
  void *ist_page_fault_top;
  void *ist_machine_check_top;
};

// one big struct to hold all the per-cpu data.
// Don't try to make global variables out of this
struct cpu_state {
  // SYSCALL entry anchor. IA32_KERNEL_GS_BASE points at this member so
  // the entry stub (interrupts.asm) can stash the user RSP and find this
  // CPU's RSP0 with gs-relative loads — SYSCALL, unlike an interrupt
  // gate, does not switch stacks. Field offsets are asm ABI: gs:[0] is
  // the scratch slot, gs:[8] the kernel stack top (same stack the TSS
  // RSP0 names). Written during cpu_setup, read only by the stub.
  struct {
    uint64_t scratch_user_rsp;
    uint64_t kernel_rsp0_top;
  } syscall_anchor;
  // logical id
  uint64_t logical_id;
  // hardware id
  uint64_t hw_id;
  // whether cpu_setup was called
  bool called_cpu_setup;
  // the current address space
  struct address_space *current_as;
  // Per-CPU runqueue + lock + saved scheduler SP. Initialized by
  // scheduler_init(); sched_rsp is filled in on the first switch out
  // of the scheduler loop.
  struct scheduler scheduler;
  // stacks for the CPU
  struct cpu_stacks stacks;
  // Nesting depth for irq_disable/irq_enable. See irq.h. Mutated only by
  // code running on this CPU with IRQs already off, so it needs no atomic.
  // Starts at 0 (the calloc in cpu_state_table_init); the first
  // irq_disable bumps it to 1 and issues the actual cli.
  uint64_t irq_depth;
};

// the only global location for CPU Data
extern size_t g_cpu_state_table_len;
extern struct cpu_state* g_cpu_state_table;


// validate that the cpu state has been set up
bool cpu_state_table_initialized();
void cpu_state_table_require();

// must be called once to allocate CPU states + interrupt stacks
void cpu_state_table_init(const struct acpi_madt *madt);

// logical "whoami" for CPUs
uint64_t cpu_state_whoami();

// This CPU's cpu_state. Fast path is one gs-relative read once
// cpu_percpu_install has run on this CPU; falls back to the hwid scan
// before that. Callers must be pinned (IRQs off or otherwise migration-
// free) for the result to stay meaningful, same as cpu_state_whoami.
struct cpu_state *cpu_state_this(void);

// Arch hooks (archsrc/<arch>/percpu.c).
// cpu_percpu_install: record this CPU's identity in a register ring 3
// cannot corrupt (tagged index in IA32_TSC_AUX on x86_64; TPIDR_EL1 on
// aarch64). cpu_percpu_try_get: this CPU's cpu_state via that register,
// or nullptr if this CPU hasn't installed yet (early boot).
void cpu_percpu_install(struct cpu_state *cs);
struct cpu_state *cpu_percpu_try_get(void);

#endif
