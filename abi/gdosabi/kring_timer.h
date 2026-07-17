#ifndef gdos_kring_timer_h_INCLUDED
#define gdos_kring_timer_h_INCLUDED

#include <stdint.h>

#include <gdosabi/kring.h>

// Scheme -5: monotonic time and one-shot absolute timers. Time values are
// nanoseconds since an unspecified boot epoch and never move backwards.
#define KSCHEME_TIMER ((int64_t)-5)

// SQE ops. Ordinary completion CQEs echo the op and arguments; KTIMER_NOW
// replaces completion.a with the sampled time.
#define KTIMER_NOW       1 // completion a = monotonic nanoseconds
#define KTIMER_ARM_ABS   2 // a = timer id, b = absolute deadline ns, c = cookie
#define KTIMER_CANCEL    3 // a = timer id

// CANCEL returns SYSERR_AGAIN when the deadline has expired and its durable
// event is waiting for CQ space. An already-posted or unknown id is INVAL.

// One-shot expiration. The event remains pending in the kernel if the CQ is
// full and is replayed after the consumer acknowledges CQ space.
#define KEV_TIMER KEV(7) // a = timer id, b = cookie

#endif // gdos_kring_timer_h_INCLUDED
