#ifndef uaccess_h_INCLUDED
#define uaccess_h_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paging.h"

struct process;

// User-memory validation for the single-address-space model. Because user
// and kernel share one identity-mapped layout, "is this pointer OK to
// touch on the user's behalf" is THE protection check in this kernel: a
// syscall that dereferences a user-supplied pointer without user_range_ok
// would let a process read or write kernel-only memory through the
// kernel's own mappings.
//
// Allocation lives in umem.h; this header is validation only.

// True iff every page of [addr, addr+len) is mapped PAGE_U | PAGE_R
// (plus PAGE_W when need_write) in the process's address space.
// len == 0 is OK; overflowing ranges are rejected.
bool user_range_ok(const struct process *p, uint64_t addr, uint64_t len,
                   bool need_write);

#endif // uaccess_h_INCLUDED
