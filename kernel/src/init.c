#include <stdint.h>

#include "efi/efi.h"
#include "efi/types.h"

static efi_status_t exit_efi_boot_services(efi_handle_t handle,
                                           struct efi_system_table *system) {

  efi_uint_t mmap_size = 4096;

  while (true) {
    struct efi_memory_descriptor *mmap;
    efi_status_t allocate_status =
        system->boot->allocate_pool(EFI_LOADER_DATA, mmap_size, (void **)&mmap);

    if (allocate_status != EFI_SUCCESS) {
      return allocate_status;
    }

    efi_uint_t desc_size;
    uint32_t desc_version;
    efi_uint_t mmap_key;
    efi_status_t mmap_status = system->boot->get_memory_map(
        &mmap_size, mmap, &mmap_key, &desc_size, &desc_version);

    if (mmap_status == EFI_SUCCESS) {
      efi_status_t exit_status =
          system->boot->exit_boot_services(handle, mmap_key);
      if (exit_status != EFI_SUCCESS) {
        system->boot->free_pool(mmap);
      }
      return exit_status;
    } else if (mmap_status == EFI_BUFFER_TOO_SMALL) {
      // If the buffer size turned out too small then get_memory_map
      // should have updated mmap_size to contain the buffer size
      // needed for the memory map. However subsequent free_pool and
      // allocate_pool might change the memory map and therefore I
      // additionally multiply it by 2.
      system->boot->free_pool(mmap);
      mmap_size *= 2;
    } else {
      system->boot->free_pool(mmap);
      return mmap_status;
    }
  }
}

efi_status_t efi_main(efi_handle_t handle, struct efi_system_table *system) {
  for (uint32_t i = 0; i < 10; i++) {
    system->out->output_string(system->out, L"hello\r\n");
  }

  exit_efi_boot_services(handle, system);

  return EFI_SUCCESS;
}
