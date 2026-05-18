#include "enumerate_cpus.h"
#include "madt_x86.h"

#include <stddef.h>
#include <stdint.h>

// Walk the MADT and emit one struct cpu per enabled/online-capable Local APIC.
// x2APIC (Type 9) entries are ignored — APIC IDs > 254 won't fit our table.
struct cpu_list enumerate_cpus(const struct acpi_madt *madt) {
  struct cpu_list list = {0};

  if (madt == nullptr) {
    return list;
  }

  const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
  const uint8_t *end = (const uint8_t *)madt + madt->header.length;

  while (p + sizeof(struct acpi_madt_entry_header) <= end) {
    const struct acpi_madt_entry_header *eh =
        (const struct acpi_madt_entry_header *)p;

    // A zero-length entry would loop forever; an over-long one is malformed.
    if (eh->length < sizeof(*eh) || p + eh->length > end) {
      break;
    }

    if (eh->type == ACPI_MADT_LOCAL_APIC) {
      const struct acpi_madt_local_apic *lapic =
          (const struct acpi_madt_local_apic *)p;
      bool enabled = (lapic->flags & 0x1) != 0;
      bool online_capable = (lapic->flags & 0x2) != 0;
      if ((enabled || online_capable) && list.count < MAX_CPUS) {
        list.cpus[list.count++] = (struct cpu){
            .hw_id = lapic->apic_id,
            .enabled = enabled,
            .online_capable = online_capable,
        };
      }
    }

    p += eh->length;
  }

  return list;
}

