# Kernel time and deadlines

Status: **revised 2026-07-26.** `KSCHEME_TIMER` (scheme `-5`, implemented
2026-07-17) is deleted, and `gdosabi/kring_timer.h` with it. Timed
waiting moves into `SYS_FUTEX_WAIT`'s deadline argument
([futex-design.md](futex-design.md) §6), which serves every consumer the
scheme had — sleeps, `*_timedwait`, `poll`-family timeouts — without
timer rings, timer events, or helper threads. What remains is the clock
itself, one syscall to read it, and the per-CPU deadline machinery,
which the dispatch quantum requires regardless of how anything waits.

## The clock

All time values are unsigned nanoseconds from an unspecified boot epoch.
The BSP calibrates the TSC and LAPIC counter together against PIT
channel 2, once. Kernel reads convert the TSC to nanoseconds with
quotient/remainder arithmetic, then apply a global atomic monotonic
clamp to tolerate small cross-CPU skew.

```
SYS_GETTIME 22 // () -> monotonic ns
```

is the sole public read interface, replacing the scheme's `KTIMER_NOW`
command. There is deliberately no userspace conversion page; if the
syscall on `clock_gettime`'s hot path ever measures, a read-only page of
TSC factors is the recorded alternative. Absolute deadlines passed to
`SYS_FUTEX_WAIT` are values in this domain.

## Per-CPU deadlines

Each CPU owns one spinlock-guarded LLRB of deadline entries keyed
`(absolute deadline, tid)`, value `struct thread *`. Parked threads with
deadlines are the only entry kind ([futex-design.md](futex-design.md)
§6): inserted at park under the futex bucket, on the CPU the thread
parks on, and removed by whichever path wins the thread — waker,
expiry, or reap — before it unblocks. **No entry outlives its wait.**
Cleanup is always the winner's because a woken thread runs no kernel
exit code: `uthread_park_blocked` never returns, and a resumed thread
goes straight to ring 3 from its saved frame
([futex-design.md](futex-design.md) §6 has the full ordering). Arming
is always local, and the scheme's remote-reprogram IPI is gone —
nothing ever arms a deadline on another CPU's tree. A winner on another
CPU may remove an entry from the arming CPU's tree, but removal never
makes a tree minimum earlier, so it never reprograms a LAPIC and never
needs an IPI; a one-shot firing for an already-removed entry finds
nothing due and rearms harmlessly.

Each CPU's one-shot LAPIC deadline is:

```
min(current absolute quantum deadline, earliest parked deadline)
```

A timer-only interrupt expires entries and rearms the remainder of the
existing quantum; it never grants a fresh quantum. The scheduler idle
path removes its quantum but preserves the earliest deadline, so a
thread parked with a timeout wakes without polling. Distant deadlines
are handled by harmless early checkpoints when the 32-bit LAPIC count
saturates.

An interrupt processes at most 64 expirations; if more are
simultaneously due, the still-due tree minimum schedules another
near-immediate shot, keeping IRQ work bounded independently of how many
threads are parked. Expiry pops its due entries, drops the timer lock,
and CASes each thread's `wake_state`; a winner claims the thread — the
claim is a lifetime pin against reap — removes both its entries, and
unblocks it with `SYSERR_TIMEDOUT`, or leaves a dead process's thread
detached for reap (the arbitration is
[futex-design.md](futex-design.md) §6's).

## What the deletion removes

Timer rings and endpoints; per-endpoint timer trees with ID/cookie
arming and linear-scan cancellation; `KTIMER_ARM_ABS`, `KTIMER_CANCEL`,
`KEV_TIMER`, and the CQ-full pending list with its durable replay;
ring-destruction cancellation of armed timers; the remote-reprogram IPI;
the waiter-following migration TODO. That is most of
`kernel/src/schemes/timer.c` (~350 lines), the `kring_timer.h` ABI, the
timer helpers in `kring.c`, and their tests. Every one of those
mechanisms existed to deliver a timer expiry through a ring as an event;
with deadlines native to the wait call, no *synchronous* consumer needs
a timer event — every timed wait is its own deadline. Asynchronous
consumers remain, and are userspace's (below).

## Userspace

`clock_gettime(CLOCK_MONOTONIC)` is `SYS_GETTIME`. `nanosleep`,
`clock_nanosleep`, and every timed wait are `SYS_FUTEX_WAIT` with a
deadline — for pure sleeps, on a private word nothing wakes
([futex-design.md](futex-design.md) §8). The waitset helper threads that
converted timer events into wakes are gone with the events.

Asynchronous POSIX timers — `timer_create`, `alarm`, interval timers,
`SIGEV_THREAD`/`SIGEV_SIGNAL` delivery — need a consumer that is not a
blocked thread. The recipe is one lazy libc timer-manager thread per
process, spawned on first use, multiplexing every armed timer into
successive `SYS_FUTEX_WAIT` deadlines. Civil and realtime clock policy
remains a future userspace service; until a kernel realtime clock
exists, an absolute `CLOCK_REALTIME` wait converted to a monotonic
deadline does not track realtime clock steps made while it sleeps.
