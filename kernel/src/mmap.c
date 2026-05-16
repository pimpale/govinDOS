#include "mmap.h"

#include <stddef.h>
#include <stdint.h>

#include <efi/efi.h>
#include <efi/types.h>

#include "stdlib/stdio.h"
#include "stdlib/string.h"

// EFI may use a stride given by desc_size. This regularizes the stride back to
// sizeof(efi_memory_descriptor)
static void compact_mmap_table(struct efi_memory_descriptor *mmap,
                               const efi_uint_t mmap_size,
                               const efi_uint_t desc_size) {
  for (size_t i = 0; i < mmap_size / desc_size; i++) {
    size_t src_off = i * desc_size;
    size_t dst_off = i * sizeof(struct efi_memory_descriptor);
    memmove((uint8_t *)mmap + dst_off, (uint8_t *)mmap + src_off,
            sizeof(struct efi_memory_descriptor));
  }
}

efi_status_t get_memory_map(struct efi_system_table *system,
                            struct efi_memory_descriptor **mmap,
                            efi_uint_t *n_mmap,
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
      system->boot->free_pool(*mmap);
      mmap_size *= 2;
    } else {
      system->boot->free_pool(*mmap);
      return mmap_status;
    }
  }
}

void dump_mmap(const uint64_t n_mmap,
               const struct efi_memory_descriptor *mmap) {
  printf("NEntries%08X\r\n", (uint32_t)n_mmap);

  uint32_t n_pages = 0;
  for (int i = 0; i < n_mmap; i++) {
    if (mmap[i].type == 7) {
      n_pages += mmap[i].pages;
    }
  }

  printf("NPages %08X\r\n", n_pages);

  for (uint32_t i = 0; i < n_mmap; i++) {
    printf("MMAP %08X:\r\n", i);
    printf(" TYPE: %08X\r\n", mmap[i].type);
    printf(" PHYS_START: %016llX\r\n", (uint64_t)mmap[i].physical_start);
    printf(" VIRT_START: %016llX\r\n", (uint64_t)mmap[i].virtual_start);
    printf(" PAGES: %016llX\r\n", mmap[i].pages);
    printf(" ATTRIBUTES: %016llX\r\n", mmap[i].attributes);
  }
}
