# Kernel timer scheme

`KSCHEME_TIMER` (`-5`) supplies the mechanism needed for monotonic deadlines
without putting waitset policy in the kernel. A process may create any number
of timer rings. The shared ABI is `gdosabi/kring_timer.h`.

## Commands and events

All time values are unsigned nanoseconds from an unspecified boot epoch.
`KTIMER_NOW` completes immediately with the sampled monotonic time in `a`.
`KTIMER_ARM_ABS` registers the one-shot `(id, absolute deadline, cookie)` in
`(a, b, c)`. IDs must be unique among that endpoint's armed or CQ-blocked
timers. `KTIMER_CANCEL` removes an armed ID; it returns `SYSERR_AGAIN` when the
deadline has already expired and its event is waiting for CQ space.

Expiration posts `KEV_TIMER { a=id, b=cookie }`. CQ-full expiration is durable:
the timer moves off the hardware-deadline queue onto a pending list, and the
consumption-ack doorbell replays it once space is available. Destroying the
ring cancels all armed and pending timers before the endpoint memory is freed.

## Clock and LAPIC integration

The BSP calibrates the TSC and LAPIC counter together against PIT channel 2.
Kernel reads convert the TSC to nanoseconds with quotient/remainder arithmetic,
then apply a global atomic monotonic clamp to tolerate small cross-CPU skew.
There is deliberately no userspace conversion page: `KTIMER_NOW` is the sole
public read interface.

Timers are assigned to the CPU executing `KTIMER_ARM_ABS` and kept in a per-CPU
left-leaning red-black tree keyed by `(absolute deadline, insertion sequence)`.
Arm, cancellation, and expiry removal are `O(log n)`, minimum lookup is
allocation-free, and equal deadlines retain FIFO order. A global timer spinlock
also protects a deadline-sorted, per-endpoint list capped at
`min(ring slots, 1024)`. ID lookup, cancellation, and teardown are therefore
fixed-bounded rather than proportional to total system timer count. Each CPU's
one-shot LAPIC deadline is:

```
min(current absolute quantum deadline, earliest armed timer deadline)
```

A timer-only interrupt expires events and rearms the remainder of the existing
quantum; it never grants a fresh quantum. The scheduler idle path removes its
quantum but preserves the earliest timer, allowing a thread parked on the timer
ring to wake without polling. Distant deadlines are handled by harmless early
checkpoints when the 32-bit LAPIC count saturates.

An interrupt processes at most 64 expirations. If more are simultaneously due,
the still-due queue head schedules another near-immediate shot. This keeps IRQ
work bounded independently of the number of timer endpoints in the system.

Cancellation from the timer's owning CPU reprograms immediately. Cross-CPU
cancellation may leave the old earlier one-shot in place; its eventual
interrupt observes the updated queue and is harmless. This avoids an IPI-based
remote timer-reprogram protocol.

Userspace implements `clock_gettime(CLOCK_MONOTONIC)`, sleeps, and waitset
helper threads over this scheme. Civil/realtime clock policy remains a future
userspace service.
