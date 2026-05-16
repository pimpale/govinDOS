#include "buddy_allocator.h"

// TODO: implement https://arxiv.org/pdf/1804.03436

#include <stddef.h>
#include <stdint.h>

#define BUDDY_LEVEL_FILLED 255
#define BUDDY_LEVEL_ALLOCATED 254
#define BUDDY_LEVEL_UNUSABLE 253
#define BUDDY_LEVEL_MAX_VALID 252

#define BUDDY_STATE_UNREADY 0
#define BUDDY_STATE_READY 1

#define assert(a, b)

// DEFINITIONS:
// level: the root of a heap has level 0, it's children have level 1, etc

// order: order 0 is the smallest unit of memory that the buddy allocator can
// allocate, order 1 is 2x that size, order 2 4x, and so on.
// We can get level from order by doing n_levels - order

struct buddy_allocator_s {
  // the offset applied to the buddy_mem_* class of functions when converting
  // from an address to a page_id
  uint64_t offset;
  // the log_2(page_size). Used for the buddy_mem_* class of functions
  uint8_t page_size_log2;
  // buddy allocator state
  uint8_t state;
  // the maximum level in the heap
  uint8_t max_level;
  // has (n_levels+1)^2 -1 entries forming a binary heap
  // Key properties:
  // for the n'th node, it's parent may be found at (n-1)/2
  // for the n'th node, it's left child may be found at 2*n + 1
  // for the n'th node, it's right child may be found at 2*n + 2
  // each entry has the following properties:
  // the smallest level of any of the children of this block which are empty
  // if BUDDY_LEVEL_ALLOCATED, this is allocated to some process
  // if BUDDY_LEVEL_UNUSABLE, this block should never be used or assigned
  // if BUDDY_LEVEL_FILLED, both children are greater than ba_max_valid_level
  uint8_t heap[];
};

////////////////////////////////
/// MATH FUNCTIONS
////////////////////////////////

static inline bool uint64_is_power_of_2(uint64_t x) {
  return __builtin_popcountll(x) == 1;
}

static inline uint8_t uint64_log2(uint64_t v) {
  return 8 * (uint8_t)sizeof(uint64_t) - (uint8_t)__builtin_clzll(v) - 1;
}

static inline uint8_t uint64_ceil_log2(uint64_t v) {
  return uint64_log2(v) + !uint64_is_power_of_2(v);
}

static inline uint64_t uint64_pow2(uint8_t i) { return (uint64_t)1 << i; }

static inline uint8_t uint8_min(uint8_t a, uint8_t b) {
  if (a < b) {
    return a;
  } else {
    return b;
  }
}

////////////////////////////////
/// HEAP FUNCTIONS
////////////////////////////////

// the level of a given index
static inline uint8_t heap_level(uint64_t i) { return uint64_log2(i + 1); }

// the size of the entire heap
static inline uint64_t heap_size(uint8_t max_level) {
  return uint64_pow2(max_level + 1) - 1;
}

// the parent index of the given index
static inline uint64_t heap_parent(uint64_t i) { return (i - 1) / 2; }

// the left child of the given index
static inline uint64_t heap_left(uint64_t i) { return 2 * i + 1; }

// the right child of the given index
static inline uint64_t heap_right(uint64_t i) { return 2 * i + 2; }

// the sibling of the given index
static inline uint64_t heap_sibling(uint64_t i) {
  if (i % 2 == 1) {
    // if index is odd, then we are a left child
    // add 1 to get the right child
    return i + 1;
  } else {
    // if index is even, then we are a right child
    // subtract 1 to get the left child
    return i - 1;
  }
}

// given two children , returns what the parent's
// should be
static uint8_t parent_free_level(const struct buddy_allocator_s *ba,
                                 uint8_t a_level, uint8_t b_level) {
  // the new smallest free level is the minimum of these two blocks
  uint8_t parent = uint8_min(a_level, b_level);

  if (parent > ba->max_level) {
    return BUDDY_LEVEL_FILLED;
  } else {
    return parent;
  }
}

static void propagate(struct buddy_allocator_s *ba, uint64_t block_index) {
  // then update the on parent blocks
  while (block_index != 0) {
    uint64_t parent = heap_parent(block_index);
    uint8_t updated_parent_level = parent_free_level(
        ba, ba->heap[block_index], ba->heap[heap_sibling(block_index)]);

    // set the parent's level
    ba->heap[parent] = updated_parent_level;
    // start processing the upper one
    block_index = parent;
  }
}

// merges together free blocks starting at block index.
// returns the bock at which coalescing is not possible anymore
static uint64_t coalesce(struct buddy_allocator_s *ba, uint64_t block_index) {
  if (ba->heap[block_index] != heap_level(block_index)) {
    return block_index;
  }

  // try to merge blocks as much as we can
  while (block_index != 0) {
    uint8_t sibling_level = ba->heap[heap_sibling(block_index)];

    if (sibling_level == heap_level(block_index)) {
      uint64_t parent = heap_parent(block_index);
      ba->heap[parent] = heap_level(parent);
      block_index = parent;
    } else {
      break;
    }
  }
  return block_index;
}

// splits blocks to find an empty slot.
// Must ensure that space exists first, or will fail
static uint64_t acquire_empty_slot(struct buddy_allocator_s *ba,
                                   const uint8_t allocation_level) {

  assert(allocation_level <= ba->max_level,
         "must have allocation level less than or equal to the max");

  uint64_t index = 0;
  uint8_t level = 0;
  while (true) {
    assert(allocation_level >= ba->heap[index],
           "must ensure that space exists before calling this function");

    // this entire block is free
    if (ba->heap[index] == level) {
      if (ba->heap[index] == allocation_level) {
        // we found a free block that has the allocation level we desire and is
        // wholly unallocated!
        return index;
      } else {
        // split block (the smallest level is now one of the children)
        ba->heap[index] = level + 1;
        ba->heap[heap_left(index)] = level + 1;
        ba->heap[heap_right(index)] = level + 1;
      }
    }

    const uint64_t left_index = heap_left(index);
    const uint64_t right_index = heap_right(index);
    const uint8_t left_level = ba->heap[left_index];
    const uint8_t right_level = ba->heap[right_index];

    // pick the one with the larger level (smaller free block) so that we
    // preserve larger blocks for potential larger allocations
    if (left_level < right_level) {
      // if fits in the right level select that one
      if (allocation_level >= right_level) {
        index = right_index;
      } else {
        index = left_index;
      }
    } else {
      // if fits in the left level select that one
      if (allocation_level >= left_level) {
        index = left_index;
      } else {
        index = right_index;
      }
    }
    level++;
  }
}

// given an index into the heap, returns the index of the first page
static uint64_t
get_first_page_index_from_block_index(struct buddy_allocator_s *ba,
                                      uint64_t block_index) {
  return (block_index - heap_size(heap_level(block_index) - 1))
         << (ba->max_level - heap_level(block_index));
}

// given an index into the heap, returns the index of the last page
static uint64_t
get_last_page_index_from_block_index(struct buddy_allocator_s *ba,
                                     uint64_t block_index) {
  return ((block_index - heap_size(heap_level(block_index) - 1) + 1)
          << (ba->max_level - heap_level(block_index))) -
         1;
}

// given the index of a page, gets the allocation it belongs to
static buddy_status_t
get_block_index_from_page_index(struct buddy_allocator_s *ba,
                                const uint64_t page_id, uint64_t *block_index) {
  uint64_t bi = 0;
  for (uint8_t level = 0; level <= ba->max_level; level++) {
    // check if this current block is the one
    if (ba->heap[bi] == BUDDY_LEVEL_ALLOCATED) {
      // if this block is allocated, we found it, so exit loop
      break;
    } else if (ba->heap[bi] == level) {
      // we hit a completely free block (error)
      return BUDDY_STATUS_NO_SUCH_ALLOCATION;
    } else if (ba->heap[bi] == BUDDY_LEVEL_UNUSABLE) {
      // we hit an unusable block (error)
      return BUDDY_STATUS_NO_SUCH_ALLOCATION;
    }
    if (page_id >= get_first_page_index_from_block_index(ba, heap_right(bi))) {
      bi = heap_right(bi);
    } else {
      bi = heap_left(bi);
    }
  }

  *block_index = bi;
  return BUDDY_STATUS_SUCCESS;
}

// gets the necessary number of bytes to construct the buddy allocator heap
uint64_t buddy_get_bytes(uint64_t n_pages) {
  assert(n_pages != 0, "n_pages must not be 0");

  uint8_t max_level = uint64_ceil_log2(n_pages);
  return sizeof(struct buddy_allocator_s) + heap_size(max_level);
}

void buddy_init(struct buddy_allocator_s *ba, uint64_t n_pages,
                uint64_t page_size, uint64_t offset) {
  assert(n_pages != 0, "n_pages must not be 0");
  assert(uint64_is_power_of_2(page_size), "page size must be a power of 2");

  ba->state = BUDDY_STATE_UNREADY;
  ba->max_level = uint64_ceil_log2(n_pages);
  ba->offset = offset;
  ba->page_size_log2 = uint64_log2(page_size);

  uint64_t bottom_level_offset = 0;
  if (ba->max_level > 0) {
    bottom_level_offset = heap_size(ba->max_level - 1);
  }

  // init the bottom level of the heap
  for (uint64_t i = bottom_level_offset; i < bottom_level_offset + n_pages;
       i++) {
    ba->heap[i] = ba->max_level;
  }
  for (uint64_t i = n_pages + bottom_level_offset; i < heap_size(ba->max_level);
       i++) {
    ba->heap[i] = BUDDY_LEVEL_UNUSABLE;
  }
}

void buddy_mark_unusable(struct buddy_allocator_s *ba, uint64_t min_page_id,
                         uint64_t max_page_id) {
  assert(ba->state == BUDDY_STATE_UNREADY,
         "allocator state is ready (should be unready)\n");
  for (uint64_t i = min_page_id; i <= max_page_id; i++) {
    ba->heap[i + uint64_pow2(ba->max_level - 1)] = BUDDY_LEVEL_UNUSABLE;
  }
}

void buddy_ready(struct buddy_allocator_s *ba) {
  if (ba->max_level > 0) {
    // walk backwards in the heap
    // start from the last block of the penultimate layer
    // compute the correct free level of the block
    // due to the way the heap is laid out, we are sure always to initialize
    // children before parents
    for (int64_t block_index = (int64_t)heap_size(ba->max_level - 1) - 1;
         block_index >= 0; block_index--) {
      uint64_t bi = (uint64_t)block_index;
      uint8_t level = heap_level(bi);
      uint8_t lv = ba->heap[heap_left(bi)];
      uint8_t rv = ba->heap[heap_right(bi)];

      if (lv == BUDDY_LEVEL_UNUSABLE && rv == BUDDY_LEVEL_UNUSABLE) {
        ba->heap[bi] = BUDDY_LEVEL_UNUSABLE;
      } else if (lv == level + 1 && rv == level + 1) {
        ba->heap[bi] = heap_level(bi);
      } else {
        ba->heap[bi] = uint8_min(lv, rv);
      }
    }
  }
  ba->state = BUDDY_STATE_READY;
}

[[nodiscard("allocations may fail")]]
buddy_status_t buddy_page_alloc(struct buddy_allocator_s *ba, uint64_t n_pages,
                                uint64_t *page_id) {
  assert(ba->state == BUDDY_STATE_READY, "allocator state is not ready\n");

  // can't allocate 0 pages, round up to 1
  if (n_pages == 0) {
    n_pages = 1;
  }

  // could never allocate
  if (n_pages > uint64_pow2(ba->max_level)) {
    return BUDDY_STATUS_INVAL;
  }

  const uint8_t allocation_level = ba->max_level - uint64_ceil_log2(n_pages);

  // we could theoretically allocate, but the structure is full
  if (allocation_level < ba->heap[0]) {
    return BUDDY_STATUS_NOMEM;
  }

  // split blocks to get a slot of the correct size
  const uint64_t block_index = acquire_empty_slot(ba, allocation_level);

  // mark this block as allocated and update parent blocks
  ba->heap[block_index] = BUDDY_LEVEL_ALLOCATED;
  // update parent blocks
  propagate(ba, block_index);

  // success
  *page_id = get_first_page_index_from_block_index(ba, block_index);
  return BUDDY_STATUS_SUCCESS;
}

buddy_status_t buddy_page_free(struct buddy_allocator_s *ba, uint64_t page_id) {
  assert(ba->state == BUDDY_STATE_READY, "allocator state is not ready\n");

  uint64_t block_index;
  buddy_status_t get_status =
      get_block_index_from_page_index(ba, page_id, &block_index);
  if (get_status != BUDDY_STATUS_SUCCESS) {
    return get_status;
  }

  // mark block as free
  ba->heap[block_index] = heap_level(block_index);
  // coalesce blocks starting from that point
  const uint64_t coalesced_block_index = coalesce(ba, block_index);
  // then update free space on the parent blocks
  propagate(ba, coalesced_block_index);

  return BUDDY_STATUS_SUCCESS;
}

static void *page_to_ptr(const struct buddy_allocator_s *ba, uint64_t page_id) {
  return (void *)(ba->offset + (page_id << ba->page_size_log2));
}

static uint64_t ptr_to_page(const struct buddy_allocator_s *ba, void *ptr) {
  return ((uint64_t)ptr - ba->offset) >> ba->page_size_log2;
}

// returns the status of the allocation. sets mem
[[nodiscard("allocations may fail")]]
buddy_status_t buddy_mem_alloc(struct buddy_allocator_s *ba, uint64_t n_bytes,
                               void **mem) {

  // minimum allocation of at least 1 page
  uint64_t page_size = uint64_pow2(ba->page_size_log2);
  if (n_bytes < page_size) {
    n_bytes = page_size;
  }

  // we do ceil log2 to get the next largest power of 2, which is guaranteed to
  // be always greater than page size
  uint64_t n_pages =
      uint64_pow2(uint64_ceil_log2(n_bytes) - ba->page_size_log2);

  uint64_t page_id;
  buddy_status_t s = buddy_page_alloc(ba, n_pages, &page_id);
  if (s != BUDDY_STATUS_SUCCESS) {
    return s;
  }

  *mem = page_to_ptr(ba, page_id);
  return BUDDY_STATUS_SUCCESS;
}

// accepts the pointer to the start of the allocation
buddy_status_t buddy_mem_free(struct buddy_allocator_s *ba, void *mem) {
  return buddy_page_free(ba, ptr_to_page(ba, mem));
}

buddy_status_t buddy_mem_size(struct buddy_allocator_s *ba, void *mem,
                              uint64_t *n_bytes) {
  assert(ba->state == BUDDY_STATE_READY, "allocator state is not ready\n");

  uint64_t block_index;
  buddy_status_t s =
      get_block_index_from_page_index(ba, ptr_to_page(ba, mem), &block_index);
  if (s != BUDDY_STATUS_SUCCESS) {
    return s;
  }

  // bucket size = pages_in_block * page_size
  //             = 2^(max_level - heap_level(block_index)) * 2^page_size_log2
  uint8_t shift = ba->max_level - heap_level(block_index) + ba->page_size_log2;
  *n_bytes = uint64_pow2(shift);
  return BUDDY_STATUS_SUCCESS;
}