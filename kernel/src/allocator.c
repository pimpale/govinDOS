#include "allocator.h"
#include "debug.h"
#include "kmath.h"

#include <buddy_allocator/buddy_allocator.h>
#include <stdint.h>

static inline void *page_to_mem(uint64_t page_id) {
  return (void *)(page_id * PAGE_SIZE);
}

struct buddy_allocator_s *
init_allocator(const uint64_t n_mmap,
               const struct efi_memory_descriptor *mmap) {
  // find max free page
  uint64_t max_free_page = 0;
  for (uint64_t i = 0; i < n_mmap; i++) {
    uint64_t page_id = mmap[i].physical_start / PAGE_SIZE;
    if (mmap[i].type == 7) {
      max_free_page = max_u64(max_free_page, page_id);
    }
  }

  uint64_t n_pages = max_free_page + 1;

  // compute size of the allocator
  uint64_t bytes_for_allocator = buddy_get_bytes(n_pages);
  uint64_t pages_for_allocator = bytes_for_allocator / PAGE_SIZE;

  // find first block of memory that can support the allocator
  uint64_t allocator_start_page_id = UINT64_MAX;
  for (uint64_t i = 0; i < n_mmap; i++) {
    if (mmap[i].type == 7 && mmap[i].pages > pages_for_allocator) {
      allocator_start_page_id = mmap[i].physical_start / PAGE_SIZE;
      break;
    }
  }

  if (allocator_start_page_id == UINT64_MAX) {
    fatal("allocator: memory too fragmented, could not initialize heap\n");
  }

  // init buddy allocator at chosen region
  struct buddy_allocator_s *ba = page_to_mem(allocator_start_page_id);
  buddy_init(ba, n_pages, PAGE_SIZE, 0);


  // mark the region of the buddy allocator itself as unusable
  buddy_mark_unusable(ba, allocator_start_page_id,
                      allocator_start_page_id + pages_for_allocator);

  // go through and mark all unusable sections
  for (uint64_t i = 0; i < n_mmap; i++) {
    if (mmap[i].type != 7) {
      uint64_t page_id = mmap[i].physical_start / PAGE_SIZE;
      buddy_mark_unusable(ba, page_id, page_id + mmap[i].pages);
    }
  }

  // make allocator ready
  buddy_ready(ba);

  // return the allocator
  return ba;
}


