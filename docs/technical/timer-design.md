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
§6): inserted at park, on the CPU the thread parks on, and removed on
wake or expiry. Arming is therefore always local, and the scheme's
remote-reprogram IPI is gone — nothing ever arms a deadline on another
CPU's tree. A wake that beats the deadline leaves a stale entry whose
`wake_state` CAS fails when it fires; nothing is cancelled, locally or
remotely.

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
and CASes each thread's `wake_state`; a winner removes the thread's
futex node by its `wait_key` and unblocks it with `SYSERR_TIMEDOUT`
(the arbitration is [futex-design.md](futex-design.md) §6's).

## What the deletion removes

Timer rings and endpoints; per-endpoint timer trees with ID/cookie
arming and linear-scan cancellation; `KTIMER_ARM_ABS`, `KTIMER_CANCEL`,
`KEV_TIMER`, and the CQ-full pending list with its durable replay;
ring-destruction cancellation of armed timers; the remote-reprogram IPI;
the waiter-following migration TODO. That is most of
`kernel/src/schemes/timer.c` (~350 lines), the `kring_timer.h` ABI, the
timer helpers in `kring.c`, and their tests. Every one of those
mechanisms existed to deliver a timer expiry through a ring as an event;
with deadlines native to the wait call, no consumer needs a timer event
at all.

## Userspace

`clock_gettime(CLOCK_MONOTONIC)` is `SYS_GETTIME`. `nanosleep`,
`clock_nanosleep`, and every timed wait are `SYS_FUTEX_WAIT` with a
deadline — for pure sleeps, on a private word nothing wakes
([futex-design.md](futex-design.md) §8). The waitset helper threads that
converted timer events into wakes are gone with the events. Civil and
realtime clock policy remains a future userspace service.
