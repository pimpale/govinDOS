#include "cpu_state.h"
#include "allocator.h"
#include "cpu_hwid.h"
#include "debug.h"
#include "enumerate_cpus.h"
#include "string.h"

size_t g_cpu_state_table_len = 0;
struct cpu_state *g_cpu_state_table = nullptr;

bool cpu_state_table_initialized() { return g_cpu_state_table != nullptr; }

void cpu_state_table_require() {
  asserts(cpu_state_table_initialized(),
          "the cpu state table hasn't been initialized");
}

void cpu_state_table_init(const struct acpi_madt *madt) {
  struct cpu_list cl = enumerate_cpus(madt);
  g_cpu_state_table_len = cl.count;
  g_cpu_state_table = malloc_unlocked(cl.count * sizeof(struct cpu_state));
  memset(g_cpu_state_table, 0, cl.count * sizeof(struct cpu_state));
  for (size_t i = 0; i < cl.count; i++) {
    g_cpu_state_table[i].logical_id = i;
    g_cpu_state_table[i].hw_id = cl.cpus[i].hw_id;
  }
}

uint64_t cpu_state_whoami() { return cpu_state_this()->logical_id; }

struct cpu_state *cpu_state_this(void) {
  cpu_state_table_require();
  struct cpu_state *cs = cpu_percpu_try_get();
  if (cs != nullptr) {
    return cs;
  }
  // Slow path: this CPU hasn't run cpu_percpu_install yet (early boot).
  // Scan by hardware id, then install so the next lookup is O(1) —
  // nothing can un-install it afterwards (IA32_TSC_AUX is wrmsr-only).
  uint64_t hwid = cpu_hwid();
  for (size_t i = 0; i < g_cpu_state_table_len; i++) {
    if (g_cpu_state_table[i].hw_id == hwid) {
      cpu_percpu_install(&g_cpu_state_table[i]);
      return &g_cpu_state_table[i];
    }
  }
  fatal("unrecognized cpu hwid");
}