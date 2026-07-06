#include "stdlib/math.h"

#include "allocator.h"
#include "debug.h"
#include "paging.h"

struct buddy_allocator_s *g_allocator = nullptr;
struct spinlock g_allocator_lock = SPINLOCK_INITIALIZER;

static inline void *page_to_mem(uint64_t page_id) {
  return (void *)(page_id * PAGE_SIZE);
}

void allocator_require(void) {
  asserts(g_allocator != nullptr,
          "kstdlib: allocator used before initialization\n");
}

void allocator_init(uint64_t n_mmap, const struct efi_memory_descriptor *mmap) {
  // size the heap to cover up to the highest free page
  uint64_t max_free_page = 0;
  for (uint64_t i = 0; i < n_mmap; i++) {
    if (mmap[i].type == EFI_CONVENTIONAL_MEMORY) {
      uint64_t page_id = mmap[i].physical_start / PAGE_SIZE;
      max_free_page = max(max_free_page, page_id);
    }
  }

  uint64_t n_pages = max_free_page + 1;
  uint64_t bytes_for_allocator = buddy_get_bytes(n_pages);
  uint64_t pages_for_allocator = bytes_for_allocator / PAGE_SIZE;

  // find the first free region large enough to host the allocator's own heap
  uint64_t allocator_start_page_id = UINT64_MAX;
  for (uint64_t i = 0; i < n_mmap; i++) {
    if (mmap[i].type == EFI_CONVENTIONAL_MEMORY &&
        mmap[i].pages > pages_for_allocator) {
      allocator_start_page_id = mmap[i].physical_start / PAGE_SIZE;
      break;
    }
  }

  if (allocator_start_page_id == UINT64_MAX) {
    fatal("kstdlib_init: memory too fragmented, could not initialize heap\n");
  }

  struct buddy_allocator_s *ba = page_to_mem(allocator_start_page_id);
  buddy_init(ba, n_pages, PAGE_SIZE, 0);

  // mark the allocator's own backing storage as unusable
  buddy_mark_unusable(ba, allocator_start_page_id,
                      allocator_start_page_id + pages_for_allocator);

  // mark every non-conventional region as unusable
  for (uint64_t i = 0; i < n_mmap; i++) {
    if (mmap[i].type != EFI_CONVENTIONAL_MEMORY) {
      uint64_t page_id = mmap[i].physical_start / PAGE_SIZE;
      buddy_mark_unusable(ba, page_id, page_id + mmap[i].pages);
    }
  }

  buddy_ready(ba);
  g_allocator = ba;
}

static void *malloc_unlocked(size_t size) {
  allocator_require();
  if (size == 0) {
    return nullptr;
  }

  void *p;
  buddy_status_t s = buddy_mem_alloc(g_allocator, (uint64_t)size, &p);
  if (s != BUDDY_STATUS_SUCCESS) {
    return nullptr;
  }
  return p;
}

static void free_unlocked(void *ptr) {
  allocator_require();
  if (ptr == nullptr) {
    return;
  }

  buddy_status_t s = buddy_mem_free(g_allocator, ptr);
  asserts(s == BUDDY_STATUS_SUCCESS, "free: pointer not owned by allocator\n");
}