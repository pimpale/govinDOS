#ifndef timer_queue_h_INCLUDED
#define timer_queue_h_INCLUDED

#include <stdint.h>

// Timer values have exactly one owner: an endpoint's deadline tree. Once an
// expiration cannot enter the CQ, it is transformed into a timer_event owned
// by the endpoint's pending-event tree instead.
typedef struct timer {
  uint64_t id;
  uint64_t deadline_ns;
  uint64_t cookie;
} timer;

typedef struct timer_event {
  uint64_t id;
  uint64_t cookie;
} timer_event;

struct timer_key {
  uint64_t deadline_ns;
  uint64_t sequence;
};

struct ring;
typedef struct ring *ring_ptr;

// One owning timer tree per timer endpoint.
#define LLRB_NAME timer
#define LLRB_KEY struct timer_key
#define LLRB_VALUE timer
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

// CQ-blocked expirations, ordered by their original deadline and arm order.
#define LLRB_NAME timer_event
#define LLRB_KEY struct timer_key
#define LLRB_VALUE timer_event
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

// One borrowed ring entry per nonempty timer endpoint on a CPU. ring_id makes
// identical endpoint deadlines distinct without ordering unrelated pointers.
struct ring_deadline_key {
  uint64_t deadline_ns;
  uint64_t ring_id;
};

#define LLRB_NAME ring_deadline
#define LLRB_KEY struct ring_deadline_key
#define LLRB_VALUE ring_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#endif // timer_queue_h_INCLUDED
