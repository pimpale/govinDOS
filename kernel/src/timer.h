#ifndef timer_h_INCLUDED
#define timer_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

struct ring;
struct thread;
struct ksqe;

// Monotonic nanoseconds since an unspecified boot epoch.
uint64_t timer_now_ns(void);

// Per-CPU LAPIC multiplexing. Dispatch installs a fresh absolute quantum;
// idle removes it but preserves the earliest userspace timer. The interrupt
// handler expires due timers and reports whether the quantum also elapsed.
void timer_cpu_dispatch(uint64_t quantum_ns);
void timer_cpu_idle(void);
bool timer_cpu_interrupt(void);

// Scheme -5 lifecycle (called by channel.c).
uint64_t timer_exec(struct thread *curr, struct ring *ring,
                    struct ksqe *sqe);
void timer_replay(struct ring *ring);
void timer_endpoint_destroy(struct ring *ring);

#endif // timer_h_INCLUDED
