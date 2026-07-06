#ifndef allocator_h_INCLUDE
#define allocator_h_INCLUDE

#include <stdint.h>
#include <efi/efi.h>
#include "buddy_allocator/buddy_allocator.h"
#include "spinlock.h"

// a (global) pointer to the allocator
extern struct buddy_allocator_s* g_allocator;
extern struct spinlock g_allocator_lock;

// require that the global allocator has been defined
void allocator_require(void);

// Build a buddy allocator covering all EFI_CONVENTIONAL_MEMORY pages in the
// given EFI memory map and install it as the global heap. Must be called once
// during kernel init before any allocation.
void allocator_init(uint64_t n_mmap, const struct efi_memory_descriptor *mmap);

// very basic early malloc and free which doesn't lock. Users must ensure they are the only one using it
void* malloc_unlocked(size_t size);
void free_unlocked(void* ptr);

// There is deliberately no per-frame metadata table. Ownership and
// lifetime of user memory are tracked at block granularity by umem.c's
// ublock lists; access checks are the page trees themselves. Per-frame
// state earns its keep in kernels with fork-style CoW, swap, or a file
// page cache — none of which fit this identity-mapped SASOS. (If shared
// page-table subtrees ever land, a one-word-per-frame refcount returns;
// see docs/technical/memory-design.md, Appendix A.)

#endif // allocator_h_INCLUDE
