#ifndef timer_queue_h_INCLUDED
#define timer_queue_h_INCLUDED

#include <stdint.h>

struct kernel_timer;
typedef struct kernel_timer *kernel_timer_ptr;

// The sequence makes simultaneous deadlines distinct and gives them stable
// insertion order. It is assigned while holding the global timer lock.
struct timer_deadline_key {
  uint64_t deadline_ns;
  uint64_t sequence;
};

#define LLRB_NAME timer_deadline
#define LLRB_KEY struct timer_deadline_key
#define LLRB_VALUE kernel_timer_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#endif // timer_queue_h_INCLUDED
