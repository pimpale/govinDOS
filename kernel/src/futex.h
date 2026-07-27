#ifndef futex_h_INCLUDED
#define futex_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "thread.h"

// Address-keyed waiting (futex-design.md). The wait queue is a static
// table of bucket trees keyed (address, park sequence); the TCB carries
// only its key, its deadline, and the wake_state claim word (thread.h).
// Parking resolves no ublock and touches no umem structure; FUTEX_WAKE
// resolves the address to a block the caller has a view of only to gate
// the wake namespace and to dispatch kernel-channel drains.

// One owning tree per bucket: waiters on one address are contiguous and
// in FIFO order (the global park sequence is the tiebreaker), so a wake
// is a lower-bound seek plus ascending iteration.
#define LLRB_NAME futex
#define LLRB_KEY struct futex_key
#define LLRB_VALUE thread_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

// One-time init (bucket locks + trees). Call before the first user process.
void futex_init(void);

// ---------------------------------------------------------------------------
// Syscall backends (syscall.c)
// ---------------------------------------------------------------------------

// SYS_FUTEX_WAKE: kernel-channel doorbell or a count-capped FIFO wake on
// exactly `addr`. Never parks; returns the number woken (only won claims
// count) or SYSERR_INVAL without a view.
uint64_t futex_sys_wake(struct thread *curr, uint64_t addr, uint64_t count);

// SYS_FUTEX_WAIT: optional fused wake of wake_addr, then compare-and-park
// on the 32-bit word at addr with an optional absolute deadline. Parks
// via uthread_park_blocked (never returning) or returns SYSERR_AGAIN /
// SYSERR_INVAL / SYSERR_NOMEM through the live frame.
uint64_t futex_sys_wait(struct thread *curr, uint64_t addr,
                        uint64_t expected, uint64_t wake_addr,
                        uint64_t deadline);

// SYS_FUTEX_REQUEUE: move up to min(count, FUTEX_REQUEUE_BATCH) waiters
// from `from` to `to` in FIFO order if the word at `from` equals
// expected. Never parks, never wakes, allocates nothing; the only path
// that holds two bucket locks.
uint64_t futex_sys_requeue(struct thread *curr, uint64_t from, uint64_t to,
                           uint64_t expected, uint64_t count);

// ---------------------------------------------------------------------------
// Kernel-initiated wakes and the claim protocol
// ---------------------------------------------------------------------------

// Wake exactly one waiter parked on addr. IRQ-handler-safe (ring posts):
// takes a bucket, the woken thread's deadline lock, g_allocator_lock,
// and the scheduler lock — never g_umem. A lost claim tries the next
// waiter on the address rather than spending the wake.
void futex_wake_one(uint64_t addr);

// Release-publish a 32-bit value at addr under its bucket, then wake one
// waiter. The bucket hold makes the store lossless against a concurrent
// compare-and-park: a thread that parks afterward sees the value and
// never parks (thread completion, §4).
void futex_publish_wake(uint64_t addr, uint32_t value);

// Try to win a parked thread: CAS PARKED -> CLAIMED, spinning briefly
// through requeue's MOVING window. False if some other party owns it.
// The claim is a reap-visible lifetime pin; the winner must remove the
// thread's tree entries and then dispose of it exactly once (unblock,
// REAPABLE, or reap's free).
bool futex_try_claim(struct thread *t);

// Finish a deadline expiry: the caller (timer.c) won the claim and
// already removed the deadline entry. Removes the futex node and either
// unblocks the thread with SYSERR_TIMEDOUT or publishes REAPABLE.
void futex_expire_claimed(struct thread *t);

// Reap support: the caller won the claim and will free the TCB itself.
// Removes the futex node and any armed deadline entry; no unblock.
void futex_reap_claimed(struct thread *t);

#endif // futex_h_INCLUDED
