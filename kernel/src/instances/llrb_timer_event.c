#include "timer_queue.h"

static int timer_event_compare(const struct timer_key *a,
                               const struct timer_key *b) {
  if (a->deadline_ns != b->deadline_ns)
    return (a->deadline_ns > b->deadline_ns) -
           (a->deadline_ns < b->deadline_ns);
  return (a->sequence > b->sequence) - (a->sequence < b->sequence);
}

#define LLRB_NAME timer_event
#define LLRB_KEY struct timer_key
#define LLRB_VALUE timer_event
#define LLRB_COMPARE(a, b) timer_event_compare((a), (b))
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
