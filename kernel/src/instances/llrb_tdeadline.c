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
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
