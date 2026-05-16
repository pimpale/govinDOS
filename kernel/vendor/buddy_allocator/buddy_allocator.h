#ifndef buddy_allocator_h_INCLUDED
#define buddy_allocator_h_INCLUDED

#include <stdint.h>

// Opaque; full definition in buddy_allocator.c. Backing storage is provided
// by the caller and must be at least buddy_get_bytes(n_pages) in size.
struct buddy_allocator_s;

typedef enum {
  BUDDY_STATUS_SUCCESS = 0,
  BUDDY_STATUS_NOMEM,
  BUDDY_STATUS_INVAL,
  BUDDY_STATUS_NO_SUCH_ALLOCATION,
} buddy_status_t;

// Bytes of backing storage required for an allocator tracking n_pages pages.
uint64_t buddy_get_bytes(uint64_t n_pages);

// Initialize an allocator in caller-provided storage. After init the allocator
// is in the "unready" state: callers must mark unusable regions with
// buddy_mark_unusable, then call buddy_ready before allocating.
//   page_size must be a power of 2
//   offset is added when converting page_id <-> pointer in buddy_mem_*
void buddy_init(struct buddy_allocator_s *ba, uint64_t n_pages,
                uint64_t page_size, uint64_t offset);

// Mark page_ids in [min_page_id, max_page_id] (inclusive) as unusable.
// Only valid in the unready state, between buddy_init and buddy_ready.
void buddy_mark_unusable(struct buddy_allocator_s *ba, uint64_t min_page_id,
                         uint64_t max_page_id);

// Transition the allocator from unready to ready. After this call, alloc/free
// are permitted and buddy_mark_unusable is not.
void buddy_ready(struct buddy_allocator_s *ba);

// Walks the heap asserting internal invariants. Intended for debugging.
void buddy_verify(struct buddy_allocator_s *ba);

// Allocate n_pages contiguous pages. n_pages is rounded up to the next power
// of 2; the returned page_id is the first page of the allocation.
[[nodiscard("allocations may fail")]]
buddy_status_t buddy_page_alloc(struct buddy_allocator_s *ba, uint64_t n_pages,
                                uint64_t *page_id);

// Free a previously-allocated region by its starting page_id.
buddy_status_t buddy_page_free(struct buddy_allocator_s *ba, uint64_t page_id);

// Like buddy_page_alloc but sized in bytes and returning a pointer. The
// pointer is computed as offset + page_id * page_size (the offset and
// page_size given at buddy_init time).
[[nodiscard("allocations may fail")]]
buddy_status_t buddy_mem_alloc(struct buddy_allocator_s *ba, uint64_t n_bytes,
                               void **mem);

// Free a region previously returned by buddy_mem_alloc.
buddy_status_t buddy_mem_free(struct buddy_allocator_s *ba, void *mem);

// Returns the size in bytes of the allocation starting at mem. The reported
// size is the rounded-up bucket size (power-of-2 pages), not the original
// n_bytes requested.
buddy_status_t buddy_mem_size(struct buddy_allocator_s *ba, void *mem,
                              uint64_t *n_bytes);

#endif // buddy_allocator_h_INCLUDED
