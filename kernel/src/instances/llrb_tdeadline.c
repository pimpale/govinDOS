#include "timer_queue.h"

static int tdeadline_compare(const struct tdeadline_key *a,
                             const struct tdeadline_key *b) {
  if (a->deadline_ns != b->deadline_ns)
    return (a->deadline_ns > b->deadline_ns) -
           (a->deadline_ns < b->deadline_ns);
  return (a->tid > b->tid) - (a->tid < b->tid);
}

#define LLRB_NAME tdeadline
#define LLRB_KEY struct tdeadline_key
#define LLRB_VALUE thread_ptr
#define LLRB_COMPARE(a, b) tdeadline_compare((a), (b))
#define LLRB_NODE_MALLOC(size) slab_llrb_tdeadline_node_malloc(size)
#define LLRB_NODE_FREE(ptr) slab_llrb_tdeadline_node_free(ptr)
#include <llrb/llrb_impl.h>
#undef LLRB_NODE_FREE
#undef LLRB_NODE_MALLOC
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
