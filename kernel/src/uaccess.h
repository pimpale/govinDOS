#ifndef uaccess_h_INCLUDED
#define uaccess_h_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paging.h"

struct process;

// User-memory validation and allocation for the single-address-space
// model. Because user and kernel share one identity-mapped AS, "is this
// pointer OK to touch on the user's behalf" is THE protection check in
// this kernel: a syscall that dereferences a user-supplied pointer
// without user_range_ok would let a process read or write kernel-only
// memory through the kernel's own mappings.

// True iff every page of [addr, addr+len) is mapped PAGE_U | PAGE_R
// (plus PAGE_W when need_write) in the process's address space.
// len == 0 is OK; overflowing ranges are rejected.
bool user_range_ok(const struct process *p, uint64_t addr, uint64_t len,
                   bool need_write);

// Allocate `len` bytes (rounded up to whole pages) of zeroed memory for
// process `p`, mapped PAGE_U | prot. Frames are tagged FRAME_USER with
// the owning pid so umem_free can enforce ownership. Returns the base
// address (== physical, identity map) or nullptr.
void *umem_alloc(struct process *p, size_t len, paging_flags_t prot);

// Undo an umem_alloc. base/len must exactly match the allocation. The
// pages return to kernel-only R|W mapping and the frames to the buddy.
// Returns 0, or negative if the range isn't page-aligned or isn't owned
// by this process (uid/pid enforcement hook for multi-user later).
int umem_free(struct process *p, uint64_t base, size_t len);

#endif // uaccess_h_INCLUDED
