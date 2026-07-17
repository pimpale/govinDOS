#ifndef pe_h_INCLUDED
#define pe_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

struct process;

// Minimal PE32+ loader for user executables.
//
// PE is the executable format for this OS (the kernel itself is a PE/EFI
// image, so every ported compiler targets one format for both). Supported
// subset: PE32+ (x86_64), section alignment == 4 KiB, base relocations
// (DIR64), static one-module TLS, and no imports or TLS callbacks. Sections
// are mapped PAGE_U with W^X derived from section characteristics.
//
// On success writes the (relocated) entry point to *entry_out and returns
// 0; on failure returns a negative value and frees nothing was mapped.
int pe_load(struct process *p, const uint8_t *image, size_t len,
            uint64_t *entry_out, uint64_t *gs_base_out);

#endif // pe_h_INCLUDED
