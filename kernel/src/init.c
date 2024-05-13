#include <stdint.h>

#include "c_builtins.h"
#include "print.h"

#include "efi/efi.h"
#include "efi/types.h"

// EFI may use a stride given by desc_size. This regularizes the stride back to
// sizeof(efi_memory_descriptor)
static void compact_mmap_table(struct efi_memory_descriptor *mmap,
                               const efi_uint_t mmap_size,
                               const efi_uint_t desc_size) {
  for (size_t i = 0; i < mmap_size / desc_size; i++) {

    struct efi_memory_descriptor *mmap_entry =
        (struct efi_memory_descriptor *)((char *)mmap + i * desc_size);

    size_t src_off = i * desc_size;
    size_t dst_off = i * sizeof(struct efi_memory_descriptor);
    memmove((uint8_t *)mmap + dst_off, (uint8_t *)mmap + src_off,
            sizeof(struct efi_memory_descriptor));
  }
}

static efi_status_t get_memory_map(struct efi_system_table *system,
                                   // out
                                   struct efi_memory_descriptor **mmap,
                                   // out
                                   efi_uint_t *n_mmap,
                                   // out
                                   efi_uint_t *mmap_key) {

  efi_uint_t mmap_size = 4096;

  while (true) {
    efi_status_t allocate_status =
        system->boot->allocate_pool(EFI_LOADER_DATA, mmap_size, (void **)mmap);

    if (allocate_status != EFI_SUCCESS) {
      return allocate_status;
    }

    efi_uint_t desc_size;
    uint32_t desc_version;
    efi_status_t mmap_status = system->boot->get_memory_map(
        &mmap_size, *mmap, mmap_key, &desc_size, &desc_version);

    if (mmap_status == EFI_SUCCESS) {
      // compact mmap table sensibly
      compact_mmap_table(*mmap, mmap_size, desc_size);

      // write number of entries
      *n_mmap = mmap_size / desc_size;
      return EFI_SUCCESS;

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
  system->out->output_string(system->out, L"hello\r\n");

  // get memory map
  efi_uint_t n_mmap = 0;
  struct efi_memory_descriptor *mmap = NULL;
  efi_uint_t mmap_key = 0;
  efi_status_t mmap_status = get_memory_map(system, &mmap, &n_mmap, &mmap_key);
  if (mmap_status != EFI_SUCCESS) {
    system->out->output_string(system->out, L"failed to get memory map!\r\n");
    return mmap_status;
  }

  output_string(system->out, L"MMAP ");
  output_u32hex(system->out, n_mmap);
  output_string(system->out, L"\r\n");

  uint32_t n_pages = 0;
  for(int i = 0; i < n_mmap; i++) {
      if(mmap[i].type == 7) {
          n_pages += mmap[i].pages;
      }
  }

  output_string(system->out, L"NPAGES ");
  output_u32hex(system->out, n_pages);
  output_string(system->out, L"\r\n");

  return 0;

  // output_string(system->out, L"MMAP ");
  // output_u32hex(system->out, n_mmap);
  // output_string(system->out, L"\r\n");

  // for (uint32_t i = 0; i < n_mmap; i++) {
  //   output_string(system->out, L"MMAP ");
  //   output_u32hex(system->out, i);
  //   output_string(system->out, L":\r\n TYPE: ");
  //   output_u32hex(system->out, mmap[i].type);
  //   output_string(system->out, L"\r\n PHYS_START: ");
  //   output_u64hex(system->out, mmap[i].physical_start);
  //   output_string(system->out, L"\r\n VIRT_START: ");
  //   output_u64hex(system->out, mmap[i].virtual_start);
  //   output_string(system->out, L"\r\n PAGES: ");
  //   output_u64hex(system->out, mmap[i].pages);
  //   output_string(system->out, L"\r\n ATTRIBUTES: ");
  //   output_u64hex(system->out, mmap[i].attributes);
  //   output_string(system->out, L"\r\n");
  // }


  // exit boot services
  efi_status_t exit_status = system->boot->exit_boot_services(handle, mmap_key);
  if (exit_status != EFI_SUCCESS) {
    system->out->output_string(system->out, L"failed to exit boot loader!\r\n");
    return exit_status;
  }

  // set up allocator

  // set up interrupts
  // setup_interrupts();

  return EFI_SUCCESS;
}
