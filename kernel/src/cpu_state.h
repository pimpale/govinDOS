#ifndef cpu_data_h_INCLUDE
#define cpu_data_h_INCLUDE

#include <stdint.h>
#include "acpi.h"

// opaque structs
struct address_space;
struct thread;

// Per-CPU kernel stacks. The "rsp0" the CPU loads on a ring-3 -> ring-0
// transition is *not* here — that's per-thread (lives in struct thread)
// and gets written into the TSS by arch_thread_install on each context
// switch.
struct cpu_stacks {
  // Bootstrap stack used during cpu_setup. Once threading takes over, this
  // stack is abandoned in favor of the per-thread kernel stacks.
  void *kernel_bootstrap_top;
  // Per-cpu interrupt stacks (constant for the life of the cpu).
  void *ist_double_fault_top;
  void *ist_nmi_top;
  void *ist_page_fault_top;
  void *ist_machine_check_top;
};

// one big struct to hold all the per-cpu data.
// Don't try to make global variables out of this
struct cpu_state {
  // logical id
  uint64_t logical_id;
  // hardware id
  uint64_t hw_id;
  // whether cpu_setup was called
  bool called_cpu_setup;
  // the current address space
  struct address_space *current_as;
  // currently running thread on this CPU. nullptr until threading takes over.
  struct thread *current_thread;
  // this CPU's idle thread. nullptr until threading_cpu_enter() runs.
  struct thread *idle_thread;
  // opaque arch-private per-cpu pointer. On x86_64 this is the per-cpu TSS
  // pointer (so arch_thread_install can update tss->rsp0). On aarch64 it
  // would point at whatever per-cpu structure the EL0->EL1 vector reads.
  void *arch_per_cpu;
  // stacks for the CPU
  struct cpu_stacks stacks;
};

// the only global location for CPU Data
extern size_t g_cpu_state_table_len;
extern struct cpu_state* g_cpu_state_table;

// validate that the cpu state has been set up
void cpu_state_table_require();

// must be called once to allocate CPU states + interrupt stacks
void cpu_state_table_init(const struct acpi_madt *madt);

// logical "whoami" for CPUs
uint64_t cpu_state_whoami();

#endif
