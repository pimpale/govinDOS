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

Each timer endpoint owns a left-leaning red-black tree of timer values keyed by
`(absolute deadline, per-ring insertion sequence)`. Equal deadlines therefore
retain FIFO order within an endpoint. ID lookup and cancellation deliberately
scan this tree linearly; an endpoint is capped at `min(ring slots, 1024)`, so
the scan is bounded without maintaining a second owning index.

Each endpoint is assigned to its creation CPU. That CPU owns a second LLRB
containing one borrowed pointer per nonempty timer ring, keyed by
`(the ring's earliest deadline, stable ring ID)`. Arming, cancelling, or
expiring a timer updates the CPU entry only when that ring's minimum changes.
Thus the CPU finds its globally earliest timer without duplicating timer
ownership, and removing or eventually migrating a ring requires moving only
one CPU-index entry. Waiter-following migration is not implemented yet.

One timer spinlock per CPU protects the CPU ring index, the timer and pending
trees of every endpoint assigned to that CPU, and the current quantum
deadline. There is no global timer lock. Each CPU's one-shot LAPIC deadline is:

```
min(current absolute quantum deadline, earliest armed timer deadline)
```

A timer-only interrupt expires events and rearms the remainder of the existing
quantum; it never grants a fresh quantum. The scheduler idle path removes its
quantum but preserves the earliest timer, allowing a thread parked on the timer
ring to wake without polling. Distant deadlines are handled by harmless early
checkpoints when the 32-bit LAPIC count saturates.

An interrupt processes at most 64 expirations. If more are simultaneously due,
the still-due ring minimum schedules another near-immediate shot. This keeps
IRQ work bounded independently of the number of timer endpoints in the system.
If the CQ is full, an expired timer value is transformed into a completion
value owned by that endpoint's pending LLRB, ordered by the timer's original
deadline and sequence. It no longer participates in hardware deadline
selection.

Local changes reprogram the LAPIC immediately. If a remote arm creates a new
earliest deadline, it sends a timer-reprogram IPI to the endpoint's assigned
CPU so a sleeping CPU cannot miss it. Remote cancellation or destruction may
leave an obsolete earlier one-shot in place; its eventual interrupt observes
the updated hierarchy and is harmless.

Userspace implements `clock_gettime(CLOCK_MONOTONIC)`, sleeps, and waitset
helper threads over this scheme. Civil/realtime clock policy remains a future
userspace service.
