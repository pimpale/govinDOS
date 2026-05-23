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
  // logical id
  uint64_t logical_id;
  // hardware id
  uint64_t hw_id;
  // whether cpu_setup was called
  bool called_cpu_setup;
  // the current address space
  struct address_space *current_as;
  // Currently running thread on this CPU. nullptr means the per-CPU
  // scheduler loop is running (between threads). Set by the scheduler
  // before switching into a thread; cleared after the thread switches
  // back out.
  struct thread *current_thread;
  // Per-CPU runqueue + lock + saved scheduler SP. Initialized by
  // scheduler_init(); sched_rsp is filled in on the first switch out
  // of the scheduler loop.
  struct scheduler scheduler;
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
