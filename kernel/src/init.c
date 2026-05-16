#include <stdint.h>

#include "acpi.h"
#include "enumerate_cpus.h"
#include "debug.h"
#include "mmap.h"
#include "serial.h"
#include "setup_interrupts.h"

#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"

#include <efi/efi.h>
#include <efi/types.h>

efi_status_t efi_main(efi_handle_t handle, struct efi_system_table *system) {
  serial_init();

  printf("starting kernel!\n");

  // get memory map
  efi_uint_t n_mmap = 0;
  struct efi_memory_descriptor *mmap = nullptr;
  efi_uint_t mmap_key = 0;
  efi_status_t mmap_status = get_memory_map(system, &mmap, &n_mmap, &mmap_key);
  asserts(mmap_status == EFI_SUCCESS, "failed to get memory map!\n");

  // grab anything that requires boot services / the system table before we
  // throw it all away
  const struct acpi_rsdp *rsdp = acpi_init(system);
  const struct acpi_madt *madt = acpi_find_madt(rsdp);
  struct cpu_list cpus = enumerate_cpus(madt);
  printf("acpi: %u processors\n", (uint32_t)cpus.count);
  for (size_t i = 0; i < cpus.count; i++) {
    printf("  cpu[%u]: hw_id=%016llX %s%s\n", (uint32_t)i,
           cpus.cpus[i].hw_id,
           cpus.cpus[i].enabled ? "enabled " : "",
           cpus.cpus[i].online_capable ? "online_capable" : "");
  }

  // exit boot services
  efi_status_t exit_status = system->boot->exit_boot_services(handle, mmap_key);
  if (exit_status != EFI_SUCCESS) {
    printf("failed to exit boot loader!\n");
    return exit_status;
  }

  // set up interrupts
  setup_interrupts();

  // set up allocator
  kstdlib_init(n_mmap, mmap);

  asserts(false, "Exit");

  return EFI_SUCCESS;
}
