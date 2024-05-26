#ifndef allocator_h_INCLUDED
#define allocator_h_INCLUDED

#include <stdint.h>
#include <efi/types.h>
#include <buddy_allocator/buddy_allocator.h>

#define PAGE_SIZE 4096

struct buddy_allocator_s* init_allocator(const uint64_t n_mmap,
                      const struct efi_memory_descriptor *mmap);

#endif // allocator_h_INCLUDED
