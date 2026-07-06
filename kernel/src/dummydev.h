#ifndef dummydev_h_INCLUDED
#define dummydev_h_INCLUDED

#include <stdint.h>

struct thread;

// A fake blocking IO device: readers block until the device "produces" a
// value (a worker kthread that simulates latency with a yield loop). It
// exists to exercise the hard part of the single-kernel-stack model —
// park a user thread mid-syscall, resume it later with a result — before
// any real device exists. In M4 it doubles as the blocking op behind
// RING_OP_DUMMY_READ, blocking the ring's worker kthread instead.

// One-time init: spawns the producer kthread. Call after threading_init.
void dummydev_init(void);

// Blocking read for a *user* thread in syscall context. Caller must have
// saved the trap frame already (arch_uthread_save_frame); the result is
// delivered into the saved frame's rax. Never returns.
[[noreturn]] void dummydev_read_user(struct thread *curr);

// Blocking read for a kernel thread (runs on its own stack, so it can
// just thread_block). Returns the produced value.
uint64_t dummydev_read_kthread(void);

#endif // dummydev_h_INCLUDED
