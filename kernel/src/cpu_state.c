#include "cpu_state.h"
#include "cpu_hwid.h"
#include "debug.h"
#include "enumerate_cpus.h"
#include "stdlib.h"

size_t g_cpu_state_table_len = 0;
struct cpu_state *g_cpu_state_table = nullptr;

void cpu_state_table_require() {
  asserts(g_cpu_state_table != nullptr,
          "the cpu state table hasn't been initialized");
}

void cpu_state_table_init(const struct acpi_madt *madt) {
  struct cpu_list cl = enumerate_cpus(madt);
  g_cpu_state_table_len = cl.count;
  g_cpu_state_table = calloc(cl.count, sizeof(struct cpu_state));
  for (size_t i = 0; i < cl.count; i++) {
    g_cpu_state_table[i].hw_id = i;
    g_cpu_state_table[i].hw_id = cl.cpus[i].hw_id;
  }
}

uint64_t cpu_state_whoami() {
  cpu_state_table_require();
  uint64_t hwid = cpu_hwid();
  for (size_t i = 0; i < g_cpu_state_table_len; i++) {
    if (g_cpu_state_table[i].hw_id == hwid) {
      return i;
    }
  }
  fatal("unrecognized cpu hwid");
}