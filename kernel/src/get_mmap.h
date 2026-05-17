#ifndef get_mmap_h_INCLUDED
#define get_mmap_h_INCLUDED

#include <stdint.h>

#include <efi/efi.h>
#include <efi/types.h>

// Calls EFI's get_memory_map (sizing the buffer as needed) and compacts the
// result so descriptors are sizeof(struct efi_memory_descriptor) apart rather
// than the firmware's desc_size stride. The resulting buffer is allocated via
// boot services and remains valid across exit_boot_services. The mmap_key is
// the value to pass to exit_boot_services.
efi_status_t get_memory_map(struct efi_system_table *system,
                            struct efi_memory_descriptor **mmap,
                            efi_uint_t *n_mmap,
                            efi_uint_t *mmap_key);

void dump_mmap(uint64_t n_mmap, const struct efi_memory_descriptor *mmap);

#endif // get_mmap_h_INCLUDED
