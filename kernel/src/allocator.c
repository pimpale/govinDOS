#include "stdlib/math.h"
#include "stdlib/string.h"

#include "allocator.h"
#include "debug.h"

struct buddy_allocator_s *g_allocator = nullptr;

struct frame_info *g_frames = nullptr;
uint64_t           g_n_frames = 0;

static inline void *page_to_mem(uint64_t page_id) {
  return (void *)(page_id * PAGE_SIZE);
}

void allocator_require(void) {
  asserts(g_allocator != nullptr,
          "kstdlib: allocator used before initialization\n");
}

struct frame_info *frame_for(uint64_t pa) {
  uint64_t pfn = pa / PAGE_SIZE;
  if (pfn >= g_n_frames) {
    return nullptr;
  }
  return &g_frames[pfn];
}

unsigned frame_get(uint64_t pa) {
  struct frame_info *fi = frame_for(pa);
  asserts(fi != nullptr, "frame_get: pa out of range\n");
  return __atomic_add_fetch(&fi->refcount, 1, __ATOMIC_SEQ_CST);
}

unsigned frame_put(uint64_t pa) {
  struct frame_info *fi = frame_for(pa);
  asserts(fi != nullptr, "frame_put: pa out of range\n");
  return __atomic_sub_fetch(&fi->refcount, 1, __ATOMIC_SEQ_CST);
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

  // Now build the frame metadata table. Its backing comes from the buddy
  // (so it has to come up after buddy_ready), and we then mark the frames
  // backing both the buddy and the table itself as FRAME_KERNEL so they
  // are never confused for free / available frames by future bookkeeping.
  uint64_t frame_info_bytes = n_pages * sizeof(struct frame_info);
  uint64_t fi_pages = (frame_info_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t fi_page_id = 0;
  buddy_status_t s = buddy_page_alloc(ba, fi_pages, &fi_page_id);
  asserts(s == BUDDY_STATUS_SUCCESS,
          "allocator: out of memory for frame_info\n");

  g_frames = page_to_mem(fi_page_id);
  g_n_frames = n_pages;
  memset(g_frames, 0, frame_info_bytes);

  // Tag pages we know about. Buddy-tracked free pages stay at the
  // memset default (kind = FRAME_FREE, refcount = 0).
  for (uint64_t i = 0; i < n_mmap; i++) {
    if (mmap[i].type == EFI_CONVENTIONAL_MEMORY) {
      continue;
    }
    uint64_t start_pfn = mmap[i].physical_start / PAGE_SIZE;
    for (uint64_t j = 0; j < mmap[i].pages; j++) {
      uint64_t pfn = start_pfn + j;
      if (pfn < g_n_frames) {
        g_frames[pfn].kind = FRAME_RESERVED;
      }
    }
  }
  for (uint64_t i = 0; i < pages_for_allocator; i++) {
    uint64_t pfn = allocator_start_page_id + i;
    if (pfn < g_n_frames) {
      g_frames[pfn].kind = FRAME_KERNEL;
    }
  }
  for (uint64_t i = 0; i < fi_pages; i++) {
    uint64_t pfn = fi_page_id + i;
    if (pfn < g_n_frames) {
      g_frames[pfn].kind = FRAME_KERNEL;
    }
  }
}
