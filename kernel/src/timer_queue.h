#ifndef timer_queue_h_INCLUDED
#define timer_queue_h_INCLUDED

#include <stdint.h>

#include "thread.h"

// Per-CPU deadline tree (timer-design.md). Parked threads with deadlines
// are the only entry kind: inserted at park under the futex bucket on
// the parking CPU, removed by whichever path wins the thread — waker,
// expiry, or reap — before it unblocks. No entry outlives its wait.
struct tdeadline_key {
  uint64_t deadline_ns;
  uint64_t tid;
};

#define LLRB_NAME tdeadline
#define LLRB_KEY struct tdeadline_key
#define LLRB_VALUE thread_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_tdeadline_node
#define SLAB_TYPE llrb_tdeadline_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#endif // timer_queue_h_INCLUDED
