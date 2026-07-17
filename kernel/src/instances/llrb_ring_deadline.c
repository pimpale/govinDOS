#include "timer_queue.h"

static int ring_deadline_compare(const struct ring_deadline_key *a,
                                 const struct ring_deadline_key *b) {
  if (a->deadline_ns != b->deadline_ns)
    return (a->deadline_ns > b->deadline_ns) -
           (a->deadline_ns < b->deadline_ns);
  return (a->ring_id > b->ring_id) - (a->ring_id < b->ring_id);
}

#define LLRB_NAME ring_deadline
#define LLRB_KEY struct ring_deadline_key
#define LLRB_VALUE ring_ptr
#define LLRB_COMPARE(a, b) ring_deadline_compare((a), (b))
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
