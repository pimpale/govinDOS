#include "timer_queue.h"

static int timer_deadline_compare(const struct timer_deadline_key *a,
                                  const struct timer_deadline_key *b) {
  if (a->deadline_ns != b->deadline_ns)
    return (a->deadline_ns > b->deadline_ns) -
           (a->deadline_ns < b->deadline_ns);
  return (a->sequence > b->sequence) - (a->sequence < b->sequence);
}

#define LLRB_NAME timer_deadline
#define LLRB_KEY struct timer_deadline_key
#define LLRB_VALUE kernel_timer_ptr
#define LLRB_COMPARE(a, b) timer_deadline_compare((a), (b))
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
