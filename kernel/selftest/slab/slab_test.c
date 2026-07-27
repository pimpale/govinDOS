#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef SLAB_TEST_PTHREAD
#include <pthread.h>
#else
#include <threads.h>
#endif

typedef struct test_object {
  uint64_t words[6];
} test_object;

typedef struct other_object {
  uint64_t words[3];
} other_object;

#define SLAB_NAME test_object
#define SLAB_TYPE test_object
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME other_object
#define SLAB_TYPE other_object
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

static _Thread_local size_t g_test_cpu;
static _Atomic size_t g_pages;
static _Atomic bool g_fail_pages;

static void *test_page_alloc(void) {
  if (atomic_load_explicit(&g_fail_pages, memory_order_relaxed))
    return NULL;
  void *page = aligned_alloc(4096, 4096);
  if (page != NULL)
    atomic_fetch_add_explicit(&g_pages, 1, memory_order_relaxed);
  return page;
}

static void test_page_free(void *page) {
  atomic_fetch_sub_explicit(&g_pages, 1, memory_order_relaxed);
  free(page);
}

#define SLAB_NAME test_object
#define SLAB_TYPE test_object
#define SLAB_PAGE_SIZE 4096
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() g_test_cpu
#define SLAB_ALLOC_PAGE() test_page_alloc()
#define SLAB_FREE_PAGE(ptr) test_page_free(ptr)
#include <slab/slab_impl.h>
#undef SLAB_FREE_PAGE
#undef SLAB_ALLOC_PAGE
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME other_object
#define SLAB_TYPE other_object
#define SLAB_PAGE_SIZE 4096
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() g_test_cpu
#define SLAB_ALLOC_PAGE() test_page_alloc()
#define SLAB_FREE_PAGE(ptr) test_page_free(ptr)
#include <slab/slab_impl.h>
#undef SLAB_FREE_PAGE
#undef SLAB_ALLOC_PAGE
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME

struct free_job {
  test_object **objects;
  size_t begin;
  size_t end;
  size_t cpu;
};

static void remote_free_run(void *arg) {
  struct free_job *job = arg;
  g_test_cpu = job->cpu;
  for (size_t i = job->begin; i < job->end; i++)
    slab_test_object_free(job->objects[i]);
}

#ifdef SLAB_TEST_PTHREAD
static void *remote_free(void *arg) {
  remote_free_run(arg);
  return NULL;
}
#else
static int remote_free(void *arg) {
  remote_free_run(arg);
  return 0;
}
#endif

int main(void) {
  enum {
    CPU_COUNT = 4,
    OBJECT_COUNT = 4096,
    WORKERS = 3,
  };

  assert(slab_test_object_init(CPU_COUNT));
  assert(slab_other_object_init(CPU_COUNT));
  assert(!slab_test_object_init(CPU_COUNT));

  g_test_cpu = 0;
  test_object **objects = calloc(OBJECT_COUNT, sizeof(*objects));
  assert(objects != NULL);
  for (size_t i = 0; i < OBJECT_COUNT; i++) {
    objects[i] = slab_test_object_zalloc(sizeof(test_object));
    assert(objects[i] != NULL);
    for (size_t j = 0; j < 6; j++)
      assert(objects[i]->words[j] == 0);
    objects[i]->words[0] = i;
  }
  assert(!slab_test_object_destroy());

  // Collecting while every object is live must leave the full pages off the
  // allocation lists; a full page on the partial list livelocks allocation.
  slab_test_object_collect();
  assert(slab_test_object_valid());

#ifdef SLAB_TEST_PTHREAD
  pthread_t threads[WORKERS];
#else
  thrd_t threads[WORKERS];
#endif
  struct free_job jobs[WORKERS];
  for (size_t i = 0; i < WORKERS; i++) {
    jobs[i] = (struct free_job){
        .objects = objects,
        .begin = OBJECT_COUNT * i / WORKERS,
        .end = OBJECT_COUNT * (i + 1) / WORKERS,
        .cpu = i + 1,
    };
#ifdef SLAB_TEST_PTHREAD
    assert(pthread_create(&threads[i], NULL, remote_free, &jobs[i]) == 0);
#else
    assert(thrd_create(&threads[i], remote_free, &jobs[i]) == thrd_success);
#endif
  }

  // Keep the owner allocating while formerly-full pages are being published
  // and drained through the remote-ready stack.
  for (size_t i = 0; i < OBJECT_COUNT * 8; i++) {
    test_object *temporary =
        slab_test_object_malloc(sizeof(test_object));
    assert(temporary != NULL);
    slab_test_object_free(temporary);
  }

  for (size_t i = 0; i < WORKERS; i++) {
#ifdef SLAB_TEST_PTHREAD
    void *result;
    assert(pthread_join(threads[i], &result) == 0);
    assert(result == NULL);
#else
    int result;
    assert(thrd_join(threads[i], &result) == thrd_success);
    assert(result == 0);
#endif
  }

  g_test_cpu = 0;
  slab_test_object_collect();
  assert(slab_test_object_valid());

  for (size_t i = 0; i < OBJECT_COUNT; i++) {
    objects[i] = slab_test_object_malloc(sizeof(test_object));
    assert(objects[i] != NULL);
  }
  for (size_t i = 0; i < OBJECT_COUNT; i++)
    slab_test_object_free(objects[i]);
  slab_test_object_collect();
  assert(slab_test_object_valid());

  // Every arena owns its own pages, while frees select the arena dynamically
  // and route cross-owner objects through the page's stored owner pointer.
  enum { PER_CPU_OBJECTS = 256 };
  test_object *per_cpu[CPU_COUNT][PER_CPU_OBJECTS];
  for (size_t cpu = 0; cpu < CPU_COUNT; cpu++) {
    g_test_cpu = cpu;
    for (size_t i = 0; i < PER_CPU_OBJECTS; i++) {
      per_cpu[cpu][i] = slab_test_object_malloc(sizeof(test_object));
      assert(per_cpu[cpu][i] != NULL);
    }
  }
  for (size_t owner = 0; owner < CPU_COUNT; owner++) {
    g_test_cpu = (owner + 1) % CPU_COUNT;
    for (size_t i = 0; i < PER_CPU_OBJECTS; i++)
      slab_test_object_free(per_cpu[owner][i]);
  }
  for (size_t cpu = 0; cpu < CPU_COUNT; cpu++) {
    g_test_cpu = cpu;
    slab_test_object_collect();
  }
  assert(slab_test_object_valid());

  g_test_cpu = 2;
  atomic_store_explicit(&g_fail_pages, true, memory_order_relaxed);
  assert(slab_other_object_malloc(sizeof(other_object)) == NULL);
  assert(slab_other_object_valid());
  atomic_store_explicit(&g_fail_pages, false, memory_order_relaxed);
  other_object *other = slab_other_object_malloc(sizeof(other_object));
  assert(other != NULL);
  slab_other_object_free(other);
  slab_other_object_collect();
  assert(slab_other_object_valid());

  assert(slab_test_object_destroy());
  assert(slab_other_object_destroy());
  assert(atomic_load_explicit(&g_pages, memory_order_relaxed) == 0);

  free(objects);
  puts("slab: hosted tests ok");
  return 0;
}
