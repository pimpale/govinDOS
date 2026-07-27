#ifndef timer_h_INCLUDED
#define timer_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

struct thread;

// Kernel time and deadlines (timer-design.md). The clock plus the
// per-CPU deadline machinery the dispatch quantum requires; parked
// threads with deadlines are the deadline trees' only entries.

// Monotonic nanoseconds since an unspecified boot epoch. SYS_GETTIME.
uint64_t timer_now_ns(void);

// Per-CPU LAPIC multiplexing. Dispatch installs a fresh absolute quantum;
// idle removes it but preserves the earliest parked deadline. The
// interrupt handler expires due waiters and reports whether the quantum
// also elapsed.
void timer_cpu_dispatch(uint64_t quantum_ns);
void timer_cpu_idle(void);
bool timer_cpu_interrupt(void);

// Arm the current thread's park deadline on THIS CPU's tree. Called by
// the futex park path with the bucket held; takes and releases the local
// timer lock (nested inside the bucket — safe because expiry drops the
// timer lock before it ever takes a bucket). Records deadline and
// deadline_cpu in the TCB and reprograms the LAPIC if the minimum moved.
// False if the tree node could not be allocated (nothing armed).
bool timer_deadline_arm(struct thread *t, uint64_t deadline_ns);

// Remove t's armed entry, if any, from its arming CPU's tree and clear
// t->deadline. Every winner of a parked thread — waker, requeue-free
// paths, reap — calls this before releasing or freeing the thread.
// Idempotent against expiry's own pop (the per-CPU timer lock serializes
// the two); removal never makes a tree minimum earlier, so it never
// reprograms a LAPIC and never needs an IPI.
void timer_deadline_cancel(struct thread *t);

#endif // timer_h_INCLUDED
