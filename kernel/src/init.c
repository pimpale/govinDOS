#include <stdint.h>

#include "acpi.h"
#include "debug.h"
#include "enumerate_cpus.h"
#include "lapic.h"
#include "madt_x86.h"
#include "get_mmap.h"
#include "serial.h"
#include "interrupts.h"
#include "allocator.h"
#include "smp.h"

#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"

#include <efi/efi.h>
#include <efi/types.h>

#define AP_TRAMPOLINE_BASE 0x8000
#define AP_STACK_SIZE      (16 * 1024)

static void ap_main(void) {
  // Each AP arrives here in long mode on the trampoline's bootstrap GDT
  // with no IDT loaded. Move it onto the kernel GDT/IDT before doing
  // anything else.
  // setup_interrupts_ap();
  uint8_t id = x86_lapic_id();
  printf("ap[%u]: hello from long mode\n", (uint32_t)id);
  cpu_signal_alive();
  for (;;) {
    __asm__ volatile("hlt");
  }
}

efi_status_t efi_main(efi_handle_t handle, struct efi_system_table *system) {
  serial_init();

  printf("starting kernel!\n");

  // Reserve the AP trampoline page at a fixed sub-1MiB physical address.
  // Pinning it through UEFI ensures (a) firmware isn't using it and (b) the
  // page comes back in the memory map as EFI_LOADER_DATA so kstdlib_init's
  // non-conventional-region pass marks it unusable in the buddy allocator.
  efi_physical_address_t trampoline_addr = AP_TRAMPOLINE_BASE;
  efi_status_t tramp_status = system->boot->allocate_pages(
      EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, 1, &trampoline_addr);
  bool have_trampoline = (tramp_status == EFI_SUCCESS);
  if (!have_trampoline) {
    printf("smp: could not reserve trampoline page at %016llX (status=%llu)\n",
           (uint64_t)AP_TRAMPOLINE_BASE, (uint64_t)tramp_status);
  }

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
  uint64_t lapic_phys = x86_lapic_address(madt);
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

  // set up allocator
  allocator_init(n_mmap, mmap);

  // Bring up the application processors.
  if (have_trampoline) {
    x86_lapic_init(lapic_phys);
    uint8_t bsp_id = x86_lapic_id();
    for (size_t i = 0; i < cpus.count; i++) {
      if (!cpus.cpus[i].enabled) {
        continue;
      }
      if (cpus.cpus[i].hw_id == bsp_id) {
        continue;
      }
      void *stack = malloc(AP_STACK_SIZE);
      asserts(stack != nullptr, "smp: failed to allocate AP stack\n");
      void *stack_top = (uint8_t *)stack + AP_STACK_SIZE;
      printf("smp: starting cpu hw_id=%llu\n", cpus.cpus[i].hw_id);
      cpu_start(cpus.cpus[i].hw_id, ap_main, stack_top);
    }
  }

  asserts(false, "Exit");

  return EFI_SUCCESS;
}
