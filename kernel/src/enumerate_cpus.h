#ifndef enumerate_cpus_h_INCLUDED
#define enumerate_cpus_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "acpi.h"

// Arch-neutral view of one processor as the kernel sees it post-enumeration.
// `hw_id` is whatever 64-bit identifier the arch uses to address a CPU:
//   - x86_64:  the LAPIC ID (zero-extended). x2APIC IDs > 254 are skipped today.
//   - aarch64: the MPIDR_EL1 affinity bits as found in the MADT GICC entry.
struct cpu {
  uint64_t hw_id;
  bool     enabled;        // OS may use this CPU now
  bool     online_capable; // OS may bring it online later
};

#define MAX_CPUS 255

struct cpu_list {
  size_t     count;
  struct cpu cpus[MAX_CPUS];
};

// Arch-implemented: decode the per-arch CPU descriptors out of the MADT.
// Lives in archsrc/<arch>/enumerate_cpus.c
struct cpu_list enumerate_cpus(const struct acpi_madt *madt);

#endif // enumerate_cpus_h_INCLUDED
