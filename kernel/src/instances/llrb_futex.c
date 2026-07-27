#include "cpu_state.h"
#include "futex.h"
#include "paging.h"

static int futex_compare(const struct futex_key *a,
                         const struct futex_key *b) {
  if (a->address != b->address)
    return (a->address > b->address) - (a->address < b->address);
  return (a->seq > b->seq) - (a->seq < b->seq);
}

#define LLRB_NAME futex
#define LLRB_KEY struct futex_key
#define LLRB_VALUE thread_ptr
#define LLRB_COMPARE(a, b) futex_compare((a), (b))
#define LLRB_TREE_MALLOC(size) slab_llrb_futex_malloc(size)
#define LLRB_TREE_FREE(ptr) slab_llrb_futex_free(ptr)
#define LLRB_NODE_MALLOC(size) slab_llrb_futex_node_malloc(size)
#define LLRB_NODE_FREE(ptr) slab_llrb_futex_node_free(ptr)
#include <llrb/llrb_impl.h>
#undef LLRB_NODE_FREE
#undef LLRB_NODE_MALLOC
#undef LLRB_TREE_FREE
#undef LLRB_TREE_MALLOC
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

// The tree slab is instantiated here, after llrb_impl.h, because this is the
// only translation unit where struct llrb_futex is complete.
#define SLAB_NAME llrb_futex
#define SLAB_TYPE llrb_futex
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME
