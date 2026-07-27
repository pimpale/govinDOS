// Fixed-type, sharded slab allocator implementation template. Include exactly
// once per SLAB_NAME from a dedicated translation unit. Required:
//
//   SLAB_NAME
//   SLAB_TYPE                 complete object type
//   SLAB_PAGE_SIZE            power-of-two backing allocation size
//   SLAB_CACHELINE_SIZE       power-of-two metadata isolation boundary
//   SLAB_WHICH_CPU()          current arena index
//
// The embedding guarantees that SLAB_WHICH_CPU() remains stable for the whole
// generated operation and that owner operations selecting one index are
// serialized. Remote frees may run concurrently.
//
// Optional:
//   SLAB_OBJECT_ALIGNMENT     defaults to alignof(SLAB_TYPE)
//   SLAB_CACHELINE_ALIGN      defaults to 0
//   SLAB_ALLOC_PAGE()         defaults to aligned_alloc(page, page)
//   SLAB_FREE_PAGE(ptr)       defaults to free(ptr)

#ifndef SLAB_NAME
#error "SLAB_NAME must be defined before including slab_impl.h"
#endif
#ifndef SLAB_TYPE
#error "SLAB_TYPE must be defined before including slab_impl.h"
#endif
#ifndef SLAB_PAGE_SIZE
#error "SLAB_PAGE_SIZE must be defined before including slab_impl.h"
#endif
#ifndef SLAB_CACHELINE_SIZE
#error "SLAB_CACHELINE_SIZE must be defined before including slab_impl.h"
#endif
#ifndef SLAB_WHICH_CPU
#error "SLAB_WHICH_CPU() must be defined before including slab_impl.h"
#endif

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <slab/slab.h>

#ifndef SLAB_OBJECT_ALIGNMENT
#define SLAB_OBJECT_ALIGNMENT alignof(SLAB_TYPE)
#define SLAB_IMPL_DEFAULT_OBJECT_ALIGNMENT
#endif
#ifndef SLAB_CACHELINE_ALIGN
#define SLAB_CACHELINE_ALIGN 0
#define SLAB_IMPL_DEFAULT_CACHELINE_ALIGN
#endif
#ifndef SLAB_ALLOC_PAGE
#define SLAB_ALLOC_PAGE() aligned_alloc(SLAB_PAGE_SIZE, SLAB_PAGE_SIZE)
#define SLAB_IMPL_DEFAULT_ALLOC_PAGE
#endif
#ifndef SLAB_FREE_PAGE
#define SLAB_FREE_PAGE(ptr) free(ptr)
#define SLAB_IMPL_DEFAULT_FREE_PAGE
#endif

#define SLAB_IMPL_PASTE_(a, b) a##b
#define SLAB_IMPL_PASTE(a, b) SLAB_IMPL_PASTE_(a, b)
#define SLAB_IMPL_T SLAB_IMPL_PASTE(slab_, SLAB_NAME)
#define SLAB_IMPL_FN(suffix) SLAB_IMPL_PASTE(SLAB_IMPL_T, suffix)
#define SLAB_IMPL_LOCAL(suffix) SLAB_IMPL_PASTE(SLAB_IMPL_FN(_impl_), suffix)
#define SLAB_IMPL_PAGE SLAB_IMPL_PASTE(SLAB_IMPL_T, _page)
#define SLAB_IMPL_ARENA SLAB_IMPL_PASTE(SLAB_IMPL_T, _arena)
#define SLAB_IMPL_STATE SLAB_IMPL_PASTE(SLAB_IMPL_T, _state)

#define SLAB_IMPL_MAX(a, b) ((a) > (b) ? (a) : (b))
#define SLAB_IMPL_ALIGN_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))

#if SLAB_CACHELINE_ALIGN
#define SLAB_IMPL_REQUESTED_ALIGNMENT                                      \
  SLAB_IMPL_MAX((size_t)SLAB_OBJECT_ALIGNMENT,                            \
                (size_t)SLAB_CACHELINE_SIZE)
#else
#define SLAB_IMPL_REQUESTED_ALIGNMENT ((size_t)SLAB_OBJECT_ALIGNMENT)
#endif

#define SLAB_IMPL_OBJECT_ALIGNMENT                                        \
  SLAB_IMPL_MAX(SLAB_IMPL_REQUESTED_ALIGNMENT, alignof(void *))
#define SLAB_IMPL_STRIDE                                                  \
  SLAB_IMPL_ALIGN_UP(SLAB_IMPL_MAX(sizeof(SLAB_TYPE), sizeof(void *)),     \
                     SLAB_IMPL_OBJECT_ALIGNMENT)

typedef struct SLAB_IMPL_PAGE SLAB_IMPL_PAGE;
typedef struct SLAB_IMPL_ARENA SLAB_IMPL_ARENA;

enum {
  SLAB_IMPL_LOCAL(PAGE_PARTIAL) = 1u << 0,
};

// The owner-mutated fields fit in the first cache line on the kernel's 64-bit
// target. Explicit alignment starts remotely mutated state on another line.
struct SLAB_IMPL_PAGE {
  void *free;
  void *local_free;
  SLAB_IMPL_PAGE *all_prev;
  SLAB_IMPL_PAGE *all_next;
  SLAB_IMPL_PAGE *partial_prev;
  SLAB_IMPL_PAGE *partial_next;
  size_t used;
  unsigned owner_flags;

  alignas(SLAB_CACHELINE_SIZE) _Atomic(uintptr_t) thread_free;
  SLAB_IMPL_PAGE *remote_next;
  SLAB_IMPL_ARENA *owner;
  const void *type_tag;
};

struct SLAB_IMPL_ARENA {
  SLAB_IMPL_PAGE *active;
  SLAB_IMPL_PAGE *partial;
  SLAB_IMPL_PAGE *all;
  SLAB_IMPL_PAGE *warm;
  size_t alloc_local;
  size_t free_local;
  size_t remote_collected;
  size_t refill_pages;

  alignas(SLAB_CACHELINE_SIZE) _Atomic(SLAB_IMPL_PAGE *) remote_ready;
};

struct SLAB_IMPL_STATE {
  SLAB_IMPL_ARENA *arenas;
  size_t arena_count;
  bool initialized;
};

static struct SLAB_IMPL_STATE SLAB_IMPL_LOCAL(state);
static const unsigned char SLAB_IMPL_LOCAL(type_tag);

#define SLAB_IMPL_OBJECT_OFFSET                                           \
  SLAB_IMPL_ALIGN_UP(sizeof(SLAB_IMPL_PAGE), SLAB_IMPL_OBJECT_ALIGNMENT)
#define SLAB_IMPL_CAPACITY                                                \
  ((SLAB_PAGE_SIZE - SLAB_IMPL_OBJECT_OFFSET) / SLAB_IMPL_STRIDE)

static_assert(SLAB_PAGE_SIZE != 0 &&
                  (SLAB_PAGE_SIZE & (SLAB_PAGE_SIZE - 1)) == 0,
              "SLAB_PAGE_SIZE must be a power of two");
static_assert(SLAB_CACHELINE_SIZE != 0 &&
                  (SLAB_CACHELINE_SIZE & (SLAB_CACHELINE_SIZE - 1)) == 0,
              "SLAB_CACHELINE_SIZE must be a power of two");
static_assert(SLAB_PAGE_SIZE % SLAB_CACHELINE_SIZE == 0,
              "slab page must be cache-line sized");
static_assert(SLAB_OBJECT_ALIGNMENT != 0 &&
                  (SLAB_OBJECT_ALIGNMENT & (SLAB_OBJECT_ALIGNMENT - 1)) == 0,
              "SLAB_OBJECT_ALIGNMENT must be a power of two");
static_assert(SLAB_IMPL_OBJECT_ALIGNMENT >= 4,
              "object alignment must leave two tag bits");
static_assert(offsetof(SLAB_IMPL_PAGE, thread_free) >=
                  SLAB_CACHELINE_SIZE,
              "remote state shares the owner cache line");
static_assert(offsetof(SLAB_IMPL_PAGE, thread_free) %
                      SLAB_CACHELINE_SIZE ==
                  0,
              "remote state is not cache-line aligned");
static_assert(alignof(SLAB_IMPL_ARENA) >= SLAB_CACHELINE_SIZE &&
                  sizeof(SLAB_IMPL_ARENA) % SLAB_CACHELINE_SIZE == 0,
              "adjacent arenas may share cache lines");
static_assert(SLAB_IMPL_OBJECT_OFFSET < SLAB_PAGE_SIZE,
              "slab header consumes its page");
static_assert(SLAB_IMPL_CAPACITY >= 2,
              "a slab instance must hold at least two objects");

enum SLAB_IMPL_LOCAL(thread_state) {
  SLAB_IMPL_LOCAL(TF_AVAILABLE) = 0,
  SLAB_IMPL_LOCAL(TF_FULL) = 1,
  SLAB_IMPL_LOCAL(TF_QUEUED) = 2,
  SLAB_IMPL_LOCAL(TF_RETIRED) = 3,
};

#define SLAB_IMPL_STATE_MASK ((uintptr_t)3)
#define SLAB_IMPL_PTR_MASK (~SLAB_IMPL_STATE_MASK)

static enum SLAB_IMPL_LOCAL(thread_state)
SLAB_IMPL_LOCAL(word_state)(uintptr_t word) {
  return (enum SLAB_IMPL_LOCAL(thread_state))(word & SLAB_IMPL_STATE_MASK);
}

static void *SLAB_IMPL_LOCAL(word_ptr)(uintptr_t word) {
  return (void *)(word & SLAB_IMPL_PTR_MASK);
}

static uintptr_t
SLAB_IMPL_LOCAL(make_word)(void *ptr,
                           enum SLAB_IMPL_LOCAL(thread_state) state) {
  assert(((uintptr_t)ptr & SLAB_IMPL_STATE_MASK) == 0);
  return (uintptr_t)ptr | (uintptr_t)state;
}

static void *SLAB_IMPL_LOCAL(next)(const void *obj) {
  return *(void *const *)obj;
}

static void SLAB_IMPL_LOCAL(set_next)(void *obj, void *next) {
  *(void **)obj = next;
}

#ifndef NDEBUG
enum { SLAB_IMPL_LOCAL(FREE_POISON) = 0xa5 };

static void SLAB_IMPL_LOCAL(poison)(void *obj) {
  memset(obj, SLAB_IMPL_LOCAL(FREE_POISON), sizeof(SLAB_TYPE));
}

static bool SLAB_IMPL_LOCAL(poison_valid)(const void *obj) {
  const unsigned char *bytes = obj;
  for (size_t i = sizeof(void *); i < sizeof(SLAB_TYPE); i++) {
    if (bytes[i] != SLAB_IMPL_LOCAL(FREE_POISON))
      return false;
  }
  return true;
}
#else
static void SLAB_IMPL_LOCAL(poison)(void *obj) { (void)obj; }
#endif

static SLAB_IMPL_ARENA *SLAB_IMPL_LOCAL(current_arena)(void) {
  assert(SLAB_IMPL_LOCAL(state).initialized);
  size_t index = (size_t)SLAB_WHICH_CPU();
  assert(index < SLAB_IMPL_LOCAL(state).arena_count);
  if (index >= SLAB_IMPL_LOCAL(state).arena_count)
    return NULL;
  return &SLAB_IMPL_LOCAL(state).arenas[index];
}

static SLAB_IMPL_PAGE *SLAB_IMPL_LOCAL(page_from_object)(const void *obj) {
  return (SLAB_IMPL_PAGE *)((uintptr_t)obj &
                           ~((uintptr_t)SLAB_PAGE_SIZE - 1));
}

static bool SLAB_IMPL_LOCAL(object_valid)(const SLAB_IMPL_PAGE *page,
                                          const void *obj) {
  if (page == NULL ||
      page->type_tag != (const void *)&SLAB_IMPL_LOCAL(type_tag) ||
      page->owner == NULL)
    return false;
  uintptr_t offset = (uintptr_t)obj - (uintptr_t)page;
  return offset >= SLAB_IMPL_OBJECT_OFFSET &&
         offset < SLAB_IMPL_OBJECT_OFFSET +
                      SLAB_IMPL_CAPACITY * SLAB_IMPL_STRIDE &&
         (offset - SLAB_IMPL_OBJECT_OFFSET) % SLAB_IMPL_STRIDE == 0;
}

static void SLAB_IMPL_LOCAL(all_add)(SLAB_IMPL_ARENA *arena,
                                     SLAB_IMPL_PAGE *page) {
  page->all_prev = NULL;
  page->all_next = arena->all;
  if (arena->all != NULL)
    arena->all->all_prev = page;
  arena->all = page;
}

static void SLAB_IMPL_LOCAL(all_remove)(SLAB_IMPL_ARENA *arena,
                                        SLAB_IMPL_PAGE *page) {
  if (page->all_prev != NULL)
    page->all_prev->all_next = page->all_next;
  else
    arena->all = page->all_next;
  if (page->all_next != NULL)
    page->all_next->all_prev = page->all_prev;
  page->all_prev = NULL;
  page->all_next = NULL;
}

static void SLAB_IMPL_LOCAL(partial_add)(SLAB_IMPL_ARENA *arena,
                                         SLAB_IMPL_PAGE *page) {
  if ((page->owner_flags & SLAB_IMPL_LOCAL(PAGE_PARTIAL)) != 0 ||
      arena->active == page)
    return;
  page->partial_prev = NULL;
  page->partial_next = arena->partial;
  if (arena->partial != NULL)
    arena->partial->partial_prev = page;
  arena->partial = page;
  page->owner_flags |= SLAB_IMPL_LOCAL(PAGE_PARTIAL);
}

static void SLAB_IMPL_LOCAL(partial_remove)(SLAB_IMPL_ARENA *arena,
                                            SLAB_IMPL_PAGE *page) {
  if ((page->owner_flags & SLAB_IMPL_LOCAL(PAGE_PARTIAL)) == 0)
    return;
  if (page->partial_prev != NULL)
    page->partial_prev->partial_next = page->partial_next;
  else
    arena->partial = page->partial_next;
  if (page->partial_next != NULL)
    page->partial_next->partial_prev = page->partial_prev;
  page->partial_prev = NULL;
  page->partial_next = NULL;
  page->owner_flags &= ~SLAB_IMPL_LOCAL(PAGE_PARTIAL);
}

static SLAB_IMPL_PAGE *SLAB_IMPL_LOCAL(partial_take)(
    SLAB_IMPL_ARENA *arena) {
  SLAB_IMPL_PAGE *page = arena->partial;
  if (page != NULL)
    SLAB_IMPL_LOCAL(partial_remove)(arena, page);
  return page;
}

static size_t SLAB_IMPL_LOCAL(adopt_list)(SLAB_IMPL_PAGE *page, void *head) {
  size_t count = 0;
  void *tail = NULL;
  for (void *obj = head; obj != NULL; obj = SLAB_IMPL_LOCAL(next)(obj)) {
    tail = obj;
    count++;
  }
  if (tail != NULL) {
    SLAB_IMPL_LOCAL(set_next)(tail, page->free);
    page->free = head;
  }
  return count;
}

static void SLAB_IMPL_LOCAL(adopt_local)(SLAB_IMPL_PAGE *page) {
  if (page->local_free == NULL)
    return;
  SLAB_IMPL_LOCAL(adopt_list)(page, page->local_free);
  page->local_free = NULL;
}

// Detach the remote object list while preserving AVAILABLE. A producer racing
// after the CAS leaves a new list for the next collection.
static size_t SLAB_IMPL_LOCAL(collect_available)(SLAB_IMPL_PAGE *page) {
  for (;;) {
    uintptr_t old =
        atomic_load_explicit(&page->thread_free, memory_order_acquire);
    assert(SLAB_IMPL_LOCAL(word_state)(old) ==
           SLAB_IMPL_LOCAL(TF_AVAILABLE));
    void *head = SLAB_IMPL_LOCAL(word_ptr)(old);
    if (head == NULL)
      return 0;
    uintptr_t desired =
        SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_AVAILABLE));
    if (!atomic_compare_exchange_weak_explicit(
            &page->thread_free, &old, desired, memory_order_acq_rel,
            memory_order_acquire))
      continue;
    size_t count = SLAB_IMPL_LOCAL(adopt_list)(page, head);
    assert(page->used >= count);
    page->used -= count;
    page->owner->remote_collected += count;
    return count;
  }
}

static void SLAB_IMPL_LOCAL(remote_ready_push)(SLAB_IMPL_ARENA *owner,
                                               SLAB_IMPL_PAGE *page) {
  SLAB_IMPL_PAGE *old =
      atomic_load_explicit(&owner->remote_ready, memory_order_relaxed);
  do {
    page->remote_next = old;
  } while (!atomic_compare_exchange_weak_explicit(
      &owner->remote_ready, &old, page, memory_order_release,
      memory_order_relaxed));
}

static void SLAB_IMPL_LOCAL(retire)(SLAB_IMPL_ARENA *arena,
                                    SLAB_IMPL_PAGE *page) {
  assert(page->used == 0);
  uintptr_t expected =
      SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_AVAILABLE));
  uintptr_t retired =
      SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_RETIRED));
  bool ok = atomic_compare_exchange_strong_explicit(
      &page->thread_free, &expected, retired, memory_order_acq_rel,
      memory_order_acquire);
  assert(ok);
  if (!ok)
    return;

  if (arena->active == page)
    arena->active = NULL;
  SLAB_IMPL_LOCAL(partial_remove)(arena, page);
  SLAB_IMPL_LOCAL(all_remove)(arena, page);
  if (arena->warm == page)
    arena->warm = NULL;
  SLAB_FREE_PAGE(page);
}

static bool SLAB_IMPL_LOCAL(handle_empty)(SLAB_IMPL_ARENA *arena,
                                          SLAB_IMPL_PAGE *page) {
  if (page->used != 0)
    return false;
  uintptr_t word =
      atomic_load_explicit(&page->thread_free, memory_order_acquire);
  if (SLAB_IMPL_LOCAL(word_state)(word) !=
          SLAB_IMPL_LOCAL(TF_AVAILABLE) ||
      SLAB_IMPL_LOCAL(word_ptr)(word) != NULL)
    return false;

  if (arena->warm == NULL || arena->warm == page) {
    arena->warm = page;
    if (arena->active == NULL) {
      SLAB_IMPL_LOCAL(partial_remove)(arena, page);
      arena->active = page;
    } else if (arena->active != page) {
      SLAB_IMPL_LOCAL(partial_add)(arena, page);
    }
    return true;
  }

  SLAB_IMPL_LOCAL(retire)(arena, page);
  return true;
}

// Consume one page whose FULL -> QUEUED transition published it to the arena.
static void SLAB_IMPL_LOCAL(collect_queued)(SLAB_IMPL_ARENA *arena,
                                           SLAB_IMPL_PAGE *page) {
  void *head;
  for (;;) {
    uintptr_t old =
        atomic_load_explicit(&page->thread_free, memory_order_acquire);
    assert(SLAB_IMPL_LOCAL(word_state)(old) == SLAB_IMPL_LOCAL(TF_QUEUED));
    head = SLAB_IMPL_LOCAL(word_ptr)(old);
    assert(head != NULL);
    uintptr_t desired =
        SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_AVAILABLE));
    if (atomic_compare_exchange_weak_explicit(
            &page->thread_free, &old, desired, memory_order_acq_rel,
            memory_order_acquire))
      break;
  }

  size_t count = SLAB_IMPL_LOCAL(adopt_list)(page, head);
  assert(page->used >= count);
  page->used -= count;
  arena->remote_collected += count;
  page->remote_next = NULL;
  SLAB_IMPL_LOCAL(adopt_local)(page);

  if (SLAB_IMPL_LOCAL(handle_empty)(arena, page))
    return;
  if (arena->active == NULL)
    arena->active = page;
  else
    SLAB_IMPL_LOCAL(partial_add)(arena, page);
}

static void SLAB_IMPL_LOCAL(drain_ready)(SLAB_IMPL_ARENA *arena) {
  SLAB_IMPL_PAGE *page = atomic_exchange_explicit(
      &arena->remote_ready, NULL, memory_order_acquire);
  while (page != NULL) {
    SLAB_IMPL_PAGE *next = page->remote_next;
    SLAB_IMPL_LOCAL(collect_queued)(arena, page);
    page = next;
  }
}

static SLAB_IMPL_PAGE *SLAB_IMPL_LOCAL(new_page)(SLAB_IMPL_ARENA *arena) {
  void *mem = SLAB_ALLOC_PAGE();
  if (mem == NULL)
    return NULL;
  if (((uintptr_t)mem & ((uintptr_t)SLAB_PAGE_SIZE - 1)) != 0) {
    SLAB_FREE_PAGE(mem);
    assert(false && "SLAB_ALLOC_PAGE returned a misaligned allocation");
    return NULL;
  }

  SLAB_IMPL_PAGE *page = mem;
  memset(page, 0, sizeof(*page));
  page->owner = arena;
  page->type_tag = &SLAB_IMPL_LOCAL(type_tag);
  atomic_init(
      &page->thread_free,
      SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_AVAILABLE)));

  unsigned char *objects =
      (unsigned char *)page + SLAB_IMPL_OBJECT_OFFSET;
  for (size_t i = SLAB_IMPL_CAPACITY; i > 0; i--) {
    void *obj = objects + (i - 1) * SLAB_IMPL_STRIDE;
    SLAB_IMPL_LOCAL(poison)(obj);
    SLAB_IMPL_LOCAL(set_next)(obj, page->free);
    page->free = obj;
  }

  SLAB_IMPL_LOCAL(all_add)(arena, page);
  arena->refill_pages++;
  return page;
}

static SLAB_TYPE *SLAB_IMPL_LOCAL(take)(SLAB_IMPL_ARENA *arena,
                                        SLAB_IMPL_PAGE *page) {
  assert(page != NULL && page->free != NULL);
  void *obj = page->free;
#ifndef NDEBUG
  assert(SLAB_IMPL_LOCAL(poison_valid)(obj));
#endif
  page->free = SLAB_IMPL_LOCAL(next)(obj);
  if (page->used == 0 && arena->warm == page)
    arena->warm = NULL;
  page->used++;
  arena->alloc_local++;
  return obj;
}

static SLAB_TYPE *SLAB_IMPL_LOCAL(malloc_slow)(SLAB_IMPL_ARENA *arena) {
  for (;;) {
    SLAB_IMPL_PAGE *page = arena->active;
    if (page != NULL) {
      SLAB_IMPL_LOCAL(adopt_local)(page);
      if (page->free == NULL) {
        uintptr_t word =
            atomic_load_explicit(&page->thread_free, memory_order_acquire);
        if (SLAB_IMPL_LOCAL(word_state)(word) ==
                SLAB_IMPL_LOCAL(TF_AVAILABLE) &&
            SLAB_IMPL_LOCAL(word_ptr)(word) != NULL)
          SLAB_IMPL_LOCAL(collect_available)(page);
      }
      if (page->free != NULL)
        return SLAB_IMPL_LOCAL(take)(arena, page);

      uintptr_t expected =
          SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_AVAILABLE));
      uintptr_t full =
          SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_FULL));
      if (!atomic_compare_exchange_strong_explicit(
              &page->thread_free, &expected, full, memory_order_acq_rel,
              memory_order_acquire))
        continue;
      assert(page->used == SLAB_IMPL_CAPACITY);
      arena->active = NULL;
    }

    SLAB_IMPL_LOCAL(drain_ready)(arena);
    if (arena->active != NULL)
      continue;

    page = SLAB_IMPL_LOCAL(partial_take)(arena);
    if (page != NULL) {
      arena->active = page;
      continue;
    }

    page = SLAB_IMPL_LOCAL(new_page)(arena);
    if (page == NULL)
      return NULL;
    arena->active = page;
  }
}

bool SLAB_IMPL_FN(_init)(size_t cpu_count) {
  if (cpu_count == 0 || SLAB_IMPL_LOCAL(state).initialized)
    return false;
  if (cpu_count > SIZE_MAX / sizeof(SLAB_IMPL_ARENA))
    return false;
  size_t bytes = cpu_count * sizeof(SLAB_IMPL_ARENA);
  size_t alignment = alignof(SLAB_IMPL_ARENA);
  if (bytes > SIZE_MAX - (alignment - 1))
    return false;
  size_t allocation_size = SLAB_IMPL_ALIGN_UP(bytes, alignment);
  SLAB_IMPL_ARENA *arenas = aligned_alloc(alignment, allocation_size);
  if (arenas == NULL)
    return false;
  memset(arenas, 0, bytes);
  for (size_t i = 0; i < cpu_count; i++)
    atomic_init(&arenas[i].remote_ready, NULL);
  SLAB_IMPL_LOCAL(state).arenas = arenas;
  SLAB_IMPL_LOCAL(state).arena_count = cpu_count;
  SLAB_IMPL_LOCAL(state).initialized = true;
  return true;
}

SLAB_TYPE *SLAB_IMPL_FN(_malloc)(size_t size) {
  if (size != sizeof(SLAB_TYPE)) {
    assert(size == sizeof(SLAB_TYPE));
    return NULL;
  }
  SLAB_IMPL_ARENA *arena = SLAB_IMPL_LOCAL(current_arena)();
  if (arena == NULL)
    return NULL;
  if (arena->active != NULL && arena->active->free != NULL)
    return SLAB_IMPL_LOCAL(take)(arena, arena->active);
  return SLAB_IMPL_LOCAL(malloc_slow)(arena);
}

SLAB_TYPE *SLAB_IMPL_FN(_zalloc)(size_t size) {
  SLAB_TYPE *obj = SLAB_IMPL_FN(_malloc)(size);
  if (obj != NULL)
    memset(obj, 0, sizeof(*obj));
  return obj;
}

static void SLAB_IMPL_LOCAL(free_remote)(SLAB_IMPL_PAGE *page, void *obj) {
  SLAB_IMPL_LOCAL(poison)(obj);
  for (;;) {
    uintptr_t old =
        atomic_load_explicit(&page->thread_free, memory_order_relaxed);
    enum SLAB_IMPL_LOCAL(thread_state) state =
        SLAB_IMPL_LOCAL(word_state)(old);
    assert(state != SLAB_IMPL_LOCAL(TF_RETIRED));
    if (state == SLAB_IMPL_LOCAL(TF_RETIRED))
      return;
    void *head = SLAB_IMPL_LOCAL(word_ptr)(old);
    SLAB_IMPL_LOCAL(set_next)(obj, head);
    enum SLAB_IMPL_LOCAL(thread_state) desired_state =
        state == SLAB_IMPL_LOCAL(TF_FULL) ? SLAB_IMPL_LOCAL(TF_QUEUED)
                                         : state;
    if (state == SLAB_IMPL_LOCAL(TF_FULL))
      assert(head == NULL);
    uintptr_t desired = SLAB_IMPL_LOCAL(make_word)(obj, desired_state);
    if (!atomic_compare_exchange_weak_explicit(
            &page->thread_free, &old, desired, memory_order_release,
            memory_order_relaxed))
      continue;
    if (state == SLAB_IMPL_LOCAL(TF_FULL))
      SLAB_IMPL_LOCAL(remote_ready_push)(page->owner, page);
    return;
  }
}

static void SLAB_IMPL_LOCAL(free_local)(SLAB_IMPL_ARENA *arena,
                                        SLAB_IMPL_PAGE *page, void *obj) {
  assert(page->used > 0);
  SLAB_IMPL_LOCAL(poison)(obj);
  SLAB_IMPL_LOCAL(set_next)(obj, page->local_free);
  page->local_free = obj;
  page->used--;
  arena->free_local++;

  uintptr_t word =
      atomic_load_explicit(&page->thread_free, memory_order_acquire);
  enum SLAB_IMPL_LOCAL(thread_state) state =
      SLAB_IMPL_LOCAL(word_state)(word);
  if (state == SLAB_IMPL_LOCAL(TF_FULL)) {
    assert(SLAB_IMPL_LOCAL(word_ptr)(word) == NULL);
    uintptr_t desired =
        SLAB_IMPL_LOCAL(make_word)(NULL, SLAB_IMPL_LOCAL(TF_AVAILABLE));
    if (atomic_compare_exchange_strong_explicit(
            &page->thread_free, &word, desired, memory_order_acq_rel,
            memory_order_acquire)) {
      if (arena->active == NULL)
        arena->active = page;
      else
        SLAB_IMPL_LOCAL(partial_add)(arena, page);
      state = SLAB_IMPL_LOCAL(TF_AVAILABLE);
    } else {
      state = SLAB_IMPL_LOCAL(word_state)(word);
    }
  }

  if (state == SLAB_IMPL_LOCAL(TF_AVAILABLE)) {
    if (arena->active != page)
      SLAB_IMPL_LOCAL(partial_add)(arena, page);
    SLAB_IMPL_LOCAL(handle_empty)(arena, page);
  }
}

void SLAB_IMPL_FN(_free)(SLAB_TYPE *obj) {
  if (obj == NULL)
    return;
  SLAB_IMPL_ARENA *current = SLAB_IMPL_LOCAL(current_arena)();
  if (current == NULL)
    return;
  SLAB_IMPL_PAGE *page = SLAB_IMPL_LOCAL(page_from_object)(obj);
  bool valid = SLAB_IMPL_LOCAL(object_valid)(page, obj);
  assert(valid);
  if (!valid)
    return;
  if (page->owner == current)
    SLAB_IMPL_LOCAL(free_local)(current, page, obj);
  else
    SLAB_IMPL_LOCAL(free_remote)(page, obj);
}

void SLAB_IMPL_FN(_collect)(void) {
  SLAB_IMPL_ARENA *arena = SLAB_IMPL_LOCAL(current_arena)();
  if (arena == NULL)
    return;
  SLAB_IMPL_LOCAL(drain_ready)(arena);
  for (SLAB_IMPL_PAGE *page = arena->all, *next; page != NULL; page = next) {
    next = page->all_next;
    uintptr_t word =
        atomic_load_explicit(&page->thread_free, memory_order_acquire);
    if (SLAB_IMPL_LOCAL(word_state)(word) ==
            SLAB_IMPL_LOCAL(TF_AVAILABLE) &&
        SLAB_IMPL_LOCAL(word_ptr)(word) != NULL)
      SLAB_IMPL_LOCAL(collect_available)(page);
    SLAB_IMPL_LOCAL(adopt_local)(page);
    if (SLAB_IMPL_LOCAL(handle_empty)(arena, page))
      continue;
    // Pages that yielded no objects are full (or queued by a racing remote
    // free) and must stay off the allocation lists.
    if (page->free != NULL && arena->active != page)
      SLAB_IMPL_LOCAL(partial_add)(arena, page);
  }
}

static bool SLAB_IMPL_LOCAL(arena_valid)(const SLAB_IMPL_ARENA *arena) {
  if (arena->active != NULL && arena->active->owner != arena)
    return false;
  if (arena->warm != NULL &&
      (arena->warm->owner != arena || arena->warm->used != 0))
    return false;
  if (atomic_load_explicit(&arena->remote_ready, memory_order_relaxed) != NULL)
    return false;

  size_t all_count = 0;
  const SLAB_IMPL_PAGE *all_prev = NULL;
  for (const SLAB_IMPL_PAGE *page = arena->all; page != NULL;
       page = page->all_next) {
    if (++all_count > SIZE_MAX / 2 || page->owner != arena ||
        page->type_tag != &SLAB_IMPL_LOCAL(type_tag) ||
        page->used > SLAB_IMPL_CAPACITY || page->all_prev != all_prev)
      return false;
    uintptr_t word =
        atomic_load_explicit(&page->thread_free, memory_order_relaxed);
    enum SLAB_IMPL_LOCAL(thread_state) state =
        SLAB_IMPL_LOCAL(word_state)(word);
    if (state == SLAB_IMPL_LOCAL(TF_RETIRED) ||
        state == SLAB_IMPL_LOCAL(TF_QUEUED))
      return false;
    bool partial =
        (page->owner_flags & SLAB_IMPL_LOCAL(PAGE_PARTIAL)) != 0;
    bool active = page == arena->active;
    if ((partial && active) || (state == SLAB_IMPL_LOCAL(TF_FULL) &&
                                (active || partial ||
                                 SLAB_IMPL_LOCAL(word_ptr)(word) != NULL ||
                                 page->used != SLAB_IMPL_CAPACITY)) ||
        (state == SLAB_IMPL_LOCAL(TF_AVAILABLE) && !active && !partial))
      return false;
    all_prev = page;
  }

  size_t partial_count = 0;
  const SLAB_IMPL_PAGE *partial_prev = NULL;
  for (const SLAB_IMPL_PAGE *page = arena->partial; page != NULL;
       page = page->partial_next) {
    if (++partial_count > all_count ||
        (page->owner_flags & SLAB_IMPL_LOCAL(PAGE_PARTIAL)) == 0 ||
        page->partial_prev != partial_prev ||
        SLAB_IMPL_LOCAL(word_state)(atomic_load_explicit(
            &page->thread_free, memory_order_relaxed)) !=
            SLAB_IMPL_LOCAL(TF_AVAILABLE))
      return false;
    partial_prev = page;
  }
  return true;
}

bool SLAB_IMPL_FN(_valid)(void) {
  if (!SLAB_IMPL_LOCAL(state).initialized)
    return false;
  for (size_t i = 0; i < SLAB_IMPL_LOCAL(state).arena_count; i++) {
    if (!SLAB_IMPL_LOCAL(arena_valid)(&SLAB_IMPL_LOCAL(state).arenas[i]))
      return false;
  }
  return true;
}

// Quiescently collect every arena without consulting SLAB_WHICH_CPU().
static void SLAB_IMPL_LOCAL(collect_arena_quiescent)(SLAB_IMPL_ARENA *arena) {
  SLAB_IMPL_LOCAL(drain_ready)(arena);
  for (SLAB_IMPL_PAGE *page = arena->all; page != NULL;
       page = page->all_next) {
    uintptr_t word =
        atomic_load_explicit(&page->thread_free, memory_order_relaxed);
    enum SLAB_IMPL_LOCAL(thread_state) state =
        SLAB_IMPL_LOCAL(word_state)(word);
    if (state == SLAB_IMPL_LOCAL(TF_AVAILABLE) &&
        SLAB_IMPL_LOCAL(word_ptr)(word) != NULL)
      SLAB_IMPL_LOCAL(collect_available)(page);
    SLAB_IMPL_LOCAL(adopt_local)(page);
  }
}

bool SLAB_IMPL_FN(_destroy)(void) {
  if (!SLAB_IMPL_LOCAL(state).initialized)
    return false;

  for (size_t i = 0; i < SLAB_IMPL_LOCAL(state).arena_count; i++)
    SLAB_IMPL_LOCAL(collect_arena_quiescent)(
        &SLAB_IMPL_LOCAL(state).arenas[i]);

  for (size_t i = 0; i < SLAB_IMPL_LOCAL(state).arena_count; i++) {
    for (SLAB_IMPL_PAGE *page = SLAB_IMPL_LOCAL(state).arenas[i].all;
         page != NULL; page = page->all_next) {
      if (page->used != 0)
        return false;
    }
  }

  for (size_t i = 0; i < SLAB_IMPL_LOCAL(state).arena_count; i++) {
    SLAB_IMPL_ARENA *arena = &SLAB_IMPL_LOCAL(state).arenas[i];
    SLAB_IMPL_PAGE *page = arena->all;
    while (page != NULL) {
      SLAB_IMPL_PAGE *next = page->all_next;
      SLAB_FREE_PAGE(page);
      page = next;
    }
  }
  free(SLAB_IMPL_LOCAL(state).arenas);
  memset(&SLAB_IMPL_LOCAL(state), 0, sizeof(SLAB_IMPL_LOCAL(state)));
  return true;
}

#ifdef SLAB_IMPL_DEFAULT_FREE_PAGE
#undef SLAB_FREE_PAGE
#undef SLAB_IMPL_DEFAULT_FREE_PAGE
#endif
#ifdef SLAB_IMPL_DEFAULT_ALLOC_PAGE
#undef SLAB_ALLOC_PAGE
#undef SLAB_IMPL_DEFAULT_ALLOC_PAGE
#endif
#ifdef SLAB_IMPL_DEFAULT_CACHELINE_ALIGN
#undef SLAB_CACHELINE_ALIGN
#undef SLAB_IMPL_DEFAULT_CACHELINE_ALIGN
#endif
#ifdef SLAB_IMPL_DEFAULT_OBJECT_ALIGNMENT
#undef SLAB_OBJECT_ALIGNMENT
#undef SLAB_IMPL_DEFAULT_OBJECT_ALIGNMENT
#endif

#undef SLAB_IMPL_PTR_MASK
#undef SLAB_IMPL_STATE_MASK
#undef SLAB_IMPL_CAPACITY
#undef SLAB_IMPL_OBJECT_OFFSET
#undef SLAB_IMPL_STRIDE
#undef SLAB_IMPL_OBJECT_ALIGNMENT
#undef SLAB_IMPL_REQUESTED_ALIGNMENT
#undef SLAB_IMPL_ALIGN_UP
#undef SLAB_IMPL_MAX
#undef SLAB_IMPL_STATE
#undef SLAB_IMPL_ARENA
#undef SLAB_IMPL_PAGE
#undef SLAB_IMPL_LOCAL
#undef SLAB_IMPL_FN
#undef SLAB_IMPL_T
#undef SLAB_IMPL_PASTE
#undef SLAB_IMPL_PASTE_
