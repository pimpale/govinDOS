#ifndef platform_mem_h_INCLUDED
#define platform_mem_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include <efi/efi.h>

#include "acpi.h"
#include "paging.h"

// Snapshot the handoff map and register firmware-described ECAM windows.
// Called once after the allocator exists and before user address spaces.
void platform_mem_init(uint64_t n_mmap,
                       const struct efi_memory_descriptor *mmap,
                       const struct acpi_rsdp *rsdp);

// Kernel-owned MMIO (IOMMU/APIC/etc.) may never become a user device view.
bool platform_mem_protect(uint64_t base, uint64_t length);

// Validate a whole page-aligned range. Firmware-table access is a distinct,
// read-only WB case; ordinary device memory is UC and excludes ECAM unless
// `allow_ecam` is true.
bool platform_mem_validate_device(uint64_t base, uint64_t length,
                                  uint32_t flags,
                                  paging_flags_t *kernel_flags_out,
                                  bool *delegatable_out);

#endif // platform_mem_h_INCLUDED
