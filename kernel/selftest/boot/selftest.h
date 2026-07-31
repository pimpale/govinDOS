#ifndef boot_selftest_h_INCLUDED
#define boot_selftest_h_INCLUDED

#include <stdint.h>

struct acpi_rsdp;

// Boot-time in-kernel selftests. Unlike the hosted suites in the sibling
// directories, these are compiled into the kernel image and run from
// efi_main before the first user process exists.
void siphash_selftest(void);
void llrb_identity_selftest(void);
void paging_merge_selftest(void);
void umem_selftest(void);
void device_block_selftest(const struct acpi_rsdp *rsdp,
                           uint64_t framebuffer_base);
void channel_selftest(void);
void process_selftest(void);

#endif // boot_selftest_h_INCLUDED
