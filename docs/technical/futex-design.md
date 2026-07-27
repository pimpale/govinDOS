# Futex design: address-keyed waiting

Status: **planned 2026-07-26.** Supersedes the `SYS_BLOCK_WAIT` /
`SYS_BLOCK_DOORBELL` pair described in
[ipc-process-design.md](ipc-process-design.md) §1, and the
"process-private events and userspace multiplexing" rules of its §2.
[park-design.md](park-design.md) is an explicit alternative to this
document — same problem, same syscall numbers, thread-directed rather
than address-keyed waiting; exactly one of the two should be built, and
its §11 is the head-to-head. It
must obey that document's design law unchanged: bounded non-blocking
kernel work, registrations for interest, events for results, **no kernel
threads, no deferred kernel work**.

Planned implementation map: `abi/gdosabi/syscall.h` (syscall numbers 11
and 12 reused in place, `SYS_GETTIME 22` and `SYS_FUTEX_REQUEUE 23`
added, plus `SYSERR_TIMEDOUT`), `kernel/src/futex.{c,h}` (new: bucket table,
park, wake, requeue), `kernel/src/channel.c` (waiter slots delete;
`channel_block_wait`/`channel_block_doorbell` delete),
`kernel/src/umem.c` (`freeing` and the resumable free delete;
`VM_UNSHARE` becomes the owner-revoke verb `(base, pid)` at a new
number, the sharer's drop renaming to `VM_DROPSHARE` in slot 9),
`kernel/src/process.c` (reap narrows
to process and thread state; futex-node removal at TCB reap),
`kernel/src/thread.h` (per-TCB wait key), `kernel/src/schemes/timer.c`
(the scheme surface deletes — [timer-design.md](timer-design.md) —
leaving the per-CPU deadline tree, repurposed for parked threads),
`packages/gdoslib-dev/kring.c` and
`packages/gdos-libc-dev/source/pthread.c` (the two consumers).

## 0. Decisions log

- **The wait queue is keyed by address, not by block.** Today's queue is
  anchored in the `ublock`, which entangles two unrelated things: the
  lifetime of user memory and the identity of a synchronization object.
  That entanglement is what generates `classify_side`, the
  private-FIFO/structural-slot duality, "wait identity", the
  empty-FIFO precondition on the first share, and the `freeing`
  protocol. Keying by address dissolves all of it: `wake(addr, n)` means
  the same thing regardless of how many sharers the containing block
  has, so **topology changes stop being identity changes.**
- **A SASOS makes this unusually cheap, and the discount is structural.**
  Both in-tree futexes need a compound key because an address alone
  names nothing: Redox keys by *physical* address and therefore carries
  a page-table walk per call, a virtual address in every entry to
  disambiguate two mappings of one frame, and weak address-space
  references to garbage-collect dead processes' entries; managarm's
  `FutexIdentity` is `{spaceQualifier, localAddress}` for the same
  reason (§1). VA == PA here, so the key is one word and all of that
  vanishes. The wait entry is a small allocated tree node; the TCB
  carries only its key and the wake-state word (§3).
- **Two of the four reference kernels already agree, and for our
  reasons.** Managarm and Redox both expose `futex_wait(addr, expected,
  deadline)` / `futex_wake(addr, count)`, and managarm puts the futex
  word **inside the kernel↔user ring** (`HelQueue.userNotify` /
  `kernelNotify`), which is §8's design running in a shipping system.
  seL4 declines futexes in favour of capability-named Notification
  objects, and MINIX declines kernel synchronization entirely — both for
  reasons that are about their own constraints rather than about futexes
  (§1).
- **Not capability-named wait objects (the seL4 shape).** A kernel
  object per synchronization primitive is fixed-size, accountable, and
  needs no user-memory load in the wait path, which is why a verified
  kernel wants it. Here it would mean an allocation and a capability per
  mutex, plus a userspace atomic layered on top anyway for the
  uncontended path, to replace state we already have: the ublock is the
  accounted object and `user_range_ok` is the gate. §9 records the
  recipe if that trade ever inverts.
- **Address is not a capability: the wake namespace is gated by view
  membership.** `FUTEX_WAKE` resolves the address to a block the caller
  has a view of, exactly as `SYS_BLOCK_DOORBELL` does today. Global
  addresses would otherwise make every futex in the system nameable by
  every process. `FUTEX_WAIT` resolves no ublock, but it must verify
  the caller's view too — without one, compare-and-park is a 32-bit
  equality oracle over every word in the SASOS. §3 does it with a
  `PAGE_U` walk of the caller's AS inside the bucket hold, so the wait
  path still leaves the umem lock hierarchy entirely.
- **The kernel wakes O(1) per event it produces.** Fan-out beyond one
  thread is only ever requested by a user thread that can be returned to
  and re-enter. This is the rule that makes an uninterruptible kernel
  with no kernel threads able to host a futex at all, and it is already
  the rule today (`ring_post_locked` wakes exactly one slot). §4.
- **One waiter representation, including for rings.** There is no
  per-topology waiter storage: a thread parked on a ring's `cq_count` is
  in the same tree as any other waiter. The cost is that an IRQ handler
  waking a ring waiter takes `g_allocator_lock` — one global spinlock
  over every kernel `malloc` and `free` — on every device interrupt.
  That is legal (a plain cli-first spinlock, never held across a
  shootdown, unlike `g_umem`) and contended, and it is accepted as a
  known scalability cost with a per-CPU slab allocator recorded in §9 as
  the fix. Redox and managarm sidestep the question by never calling
  futex from an IRQ; we cannot, because CQE posts happen in handlers.
- **`FUTEX_WAIT` fuses an optional wake.** The fourth argument
  `wake_addr` is woken before the compare-and-park. One syscall covers
  submit-then-wait on a kernel ring, submit-then-wait on a user ring,
  and unlock-then-wait for a condvar. This is what keeps the
  user↔kernel and user↔user ring loops literally the same code, which
  the previous two-verb design achieved by making the kernel infer the
  peer's role.
- **`FUTEX_CMP_REQUEUE` is in the surface, not deferred.** glibc- and
  musl-shaped condvars requeue waiters onto the mutex in
  `pthread_cond_broadcast`; without it every broadcast is a stampede —
  N wakes and N immediate re-parks on the mutex. It is the first path
  in the system to hold two bucket locks; it adds a `MOVING` state to
  the wake arbitration so a concurrent timeout cannot chase a rewritten
  `wait_key`, and an extract/insert-existing-node pair to the vendored
  LLRB so the move allocates nothing (§2) — roughly 80 lines, sequenced
  last in §10 so the base protocol is implemented and tested without
  it.
- **The kernel never wakes waiters on revocation.** A parked thread
  whose block is revoked is not notified; it recovers via its deadline
  and a re-park that fails `SYSERR_INVAL`. Orderly teardown is driven by
  the owner (close sentinel, then wake; peers `VM_DROPSHARE` as their
  ack; then free — `VM_UNSHARE(base, pid)` coerces a peer that never
  acks); disorderly teardown is driven by the parent (robust list, then
  `VM_MOVE` the memory up and free it). `SYS_VM_FREE` is a single
  bounded transaction that fails while anything is still attached. §5.
- **Robust mutex recovery is userspace's, via `SYS_SET_ROBUST_LIST`.**
  The kernel stores the registration; the parent walks the list at reap.
  Its coverage is the same as Linux's — words in the owner-TID encoding
  only — so it is a mutex-recovery mechanism, not a general peer-death
  notification.
- **Timeouts are in the wait call, from the start — and they are the
  system's only timed-wait mechanism.** The timer scheme is deleted
  alongside this work ([timer-design.md](timer-design.md)): its one
  would-be role here, a helper thread per process converting timer
  events into wakes, is the "incredibly inefficient workaround" the
  Linux-compatibility goal exists to avoid. `pthread_cond_timedwait`,
  `sem_timedwait`, `nanosleep`, and every `poll`/`epoll`/`select`
  timeout route through the deadline argument. It is not separable work: the deadline is not a feature bolted
  onto a park, it is the park state machine, and the `wake_state` CAS
  arbitration (§6) has to be present in every wake path from the
  beginning. Both in-tree futexes take the deadline in the wait call
  (`helFutexWait(pointer, expected, deadline)`, Redox's `TimeSpec`
  argument).
- **Thread completion stays an address wait for now, but is the one
  natural park/unpark case.** §4 and §9.
- **Syscall numbers 11 and 12 are reused in place.** There is no
  compatibility obligation; the old spellings simply cease to exist.

## 1. Precedents

Surveyed from the four reference trees (`references/{managarm,redoxos,
seL4,minix}`). The split is informative: **the two microkernels with
kernel-scheduled user threads and shared-memory rings both landed on
futexes, and both put the futex word inside the ring itself.** The two
that did not are the verified kernel and the one with no kernel
threading at all.

- **Managarm** (`kernel/thor/generic/thor-internal/futex.hpp`,
  `hel/include/hel.h`): the closest precedent by a wide margin. A
  `FutexRealm` holds `hash_map<FutexIdentity, Slot>`, each `Slot` an
  **intrusive list of waiter `Node`s living in the waiter's own
  frame** — a no-allocation-per-waiter property this design does not
  copy: §3 allocates a tree node per park, and trades that property
  for ordered, fair buckets. `helFutexWait(pointer, expected, deadline)` and
  `helFutexWake(pointer, count)`; note the **deadline is in the wait
  call**, which is the §6 decision independently. Two details worth
  copying and one worth not:
  - `FutexIdentity` is a **two-word key** — `{spaceQualifier,
    localAddress}`, both opaque to the futex code — precisely because
    an address is not globally meaningful. This is the sharpest
    statement of the SASOS discount available: our identity is one
    word, and neither Redox's frame translation nor managarm's
    qualifier has anything to do here.
  - `wake()` pops the batch into a local `pending` list **under the
    lock, then raises the completion events after dropping it.** Adopt
    this (§4): `thread_unblock` spins on `on_cpu`, and holding a bucket
    across up to `FUTEX_WAKE_BATCH` such spins would be a bad trade.
  - The realm is guarded by one `frg::ticket_spinlock` with a standing
    TODO: *"use a scalable hash table with fine-grained locks."*
  `struct HelQueue` is the validation of §8: an SQ/CQ ring in shared
  memory whose notification mechanism is **two futex words, one per
  direction** — `userNotify` ("futex that is used to wake userspace",
  kernel ORs bits, userspace ANDs them off) and `kernelNotify` — plus a
  per-chunk `progressFutex`. Our `cq_count` is the counter-shaped
  equivalent of `userNotify`, and `FUTEX_WAKE` on the ring base is
  `kernelNotify`. The one part that does not transfer is that managarm's
  *kernel* waits on `kernelNotify`; it has fibers, and we have the
  standing no-kernel-threads rule.
- **Redox** (`src/syscall/futex.rs`): one global `Mutex<HashMap<
  PhysicalAddress, Vec<FutexEntry>>>`. `FUTEX_WAIT` translates the
  virtual address to a frame, holds the address-space read guard across
  the value load (the same hazard our `channel_block_wait` handles by
  holding the list lock across its load), compares, blocks, and pushes
  an entry. `FUTEX_WAKE` walks the vector for that frame matching *both*
  virtual address and address space, wakes up to `val`, returns the
  count, and opportunistically drops entries whose address space is
  gone. Taken: the count-capped wake returning its count, and the
  confirmation that even a single global lock is survivable. Not taken:
  the per-key `Vec`, which reallocates on growth and removes in O(n);
  our per-park allocation is one fixed-size tree node (§3). Its own TODO is worth recording: *"futex is probably the
  best API for process-shared POSIX synchronization primitives, [but] a
  local hash table and wait-for-thread kernel APIs (e.g.
  lwp_park/lwp_unpark) could be a simpler replacement"* for
  process-**private** futexes. That is a split, not a replacement, and
  §9 keeps the door open on exactly that line.
- **seL4** (`src/object/notification.c`): **no futex, deliberately.**
  Synchronization is the Notification object — a capability-named kernel
  object with a three-state machine (Idle / Waiting / Active), a badge
  word, an **intrusive TCB queue** (`ntfnQueue_head/tail`, the same TCB
  storage trick), and an optionally bound TCB. `sendSignal` either wakes
  the head of the queue with the badge in a register, or, if nobody is
  waiting, ORs the badge into the Active state — coalescing level-state,
  structurally the same idea as our un-notified share edges and the IRQ
  `raised`/`acked` pair. `receiveSignal` consumes an Active badge or
  enqueues. This is the serious alternative to everything in this
  document, and the reasons it wins for seL4 are reasons it loses here:
  a verified kernel wants no user-memory load in its wait path and a
  fixed-size, capability-accounted object, and it accepts an object
  allocation per synchronization primitive plus a userspace atomic
  layered on top for the uncontended fast path. We already have the
  ublock as the accounted object and `user_range_ok` as the gate, and an
  object per mutex is exactly the per-primitive kernel state this design
  exists to avoid. §9 records what adopting it would mean.
- **MINIX 3** (`minix/lib/libmthread/`): **no kernel synchronization
  primitive of any kind.** Threads are user-level and cooperative —
  `scheduler.c` swaps `ucontext_t`s, `mthread_mutex_lock` appends the
  current thread to the mutex's own FIFO and calls `mthread_suspend`,
  and the whole thing never enters the kernel. Kernel-level blocking
  exists only as the synchronous `send`/`receive`/`sendrec` message
  rendezvous. (The `_lwp_park` files under `lib/libc/compat` are
  inherited NetBSD userland, not a MINIX implementation.) This is the
  true floor for kernel LoC, and it is unavailable to us for one
  concrete reason: preemptive, SMP, kernel-scheduled threads are already
  a committed feature, and `pthread.c` already spawns real ones.
- **Linux, FreeBSD `umtx`, WebKit `ParkingLot`, NetBSD `lwp_park`** (not
  in tree, from general knowledge): Linux's `FUTEX_WAIT`/`FUTEX_WAKE`
  are the emulation target, and its flat bucket chains are safe only
  because `futex_wake` never runs in interrupt context. FreeBSD's
  `umtx` is the source of §3's two-level shape (a chain of keys, each
  owning a FIFO). `ParkingLot`/`lwp_park` are the
  userspace-hashtable-over-thread-parking fallback in §9.

## 2. Syscalls

```
SYS_FUTEX_WAKE    11 // (addr, count)                      -> nwoken | SYSERR_*
SYS_FUTEX_WAIT    12 // (addr, expected, wake_addr, deadline) -> 0 | SYSERR_*
SYS_FUTEX_REQUEUE 23 // (from, to, expected, count)        -> nmoved | SYSERR_*
```

Four arguments is exactly the ABI maximum (r10, rdx, r8, r9).

**`FUTEX_WAKE(addr, count)`** — never parks. Resolves `addr` to a block
the caller has a view of (`SYSERR_INVAL` otherwise). If that block is a
kernel channel, this is the old doorbell: drain and execute the SQ in
the caller's context under the existing `RING_SQ_BATCH`/`RING_SQ_CHUNK`
bounds, run the scheme's replay, and ignore `count`. Otherwise wake up
to `min(count, FUTEX_WAKE_BATCH)` threads parked on exactly `addr`,
FIFO, and return how many. `count == UINT64_MAX` spells "as many as you
will"; the caller loops while the return equals the cap. The word at
`addr` is never read.

**`FUTEX_WAIT(addr, expected, wake_addr, deadline)`** — may park.
`addr` must be 4-byte aligned and readable in the caller's view.

1. If `wake_addr != 0`, perform `FUTEX_WAKE(wake_addr, 1)` — or the
   ring drain, if `wake_addr` names a kernel channel — and release
   everything it touched.
2. Load the 32-bit word at `addr`. If it differs from `expected`,
   return `SYSERR_AGAIN`.
3. Otherwise park until woken, revoked, or the deadline passes.

`deadline` is an absolute `SYS_GETTIME`-domain nanosecond value; 0 means
none. Returns:

| result | meaning | Linux |
|---|---|---|
| 0 | woken (or spuriously woken) | 0 |
| `SYSERR_AGAIN` | the word did not equal `expected` | `EAGAIN` |
| `SYSERR_TIMEDOUT` | the deadline passed while parked | `ETIMEDOUT` |
| `SYSERR_INVAL` | unaligned, or no view of the address | `EFAULT`/`EINVAL` |
| `SYSERR_NOMEM` | the wait node could not be allocated (§3) | — |

`SYSERR_TIMEDOUT` is a new error code (`-9`). The mismatch case reports
distinctly from a wake rather than collapsing into 0, because an
emulation layer cannot synthesize the distinction after the fact — by
the time it re-reads the word, the value may have changed again. Every
futex precedent surveyed separates them (managarm `kHelErrFutexRace`,
Redox and Linux `EAGAIN`).

The wake in step 1 completes and releases before the park in step 3
begins, so no path ever holds two bucket locks. That ordering is the
standard condvar discipline and is safe for the same reason: the caller
sampled `expected` before releasing whatever it is handing off.

**`FUTEX_REQUEUE(from, to, expected, count)`** — never parks, never
wakes. Resolves both addresses to blocks the caller has a view of
(`SYSERR_INVAL` otherwise). Loads the 32-bit word at `from`; if it
differs from `expected`, returns `SYSERR_AGAIN` — a broadcast racing a
state change must not requeue against the new state, which is Linux's
`CMP_REQUEUE` guard. Otherwise moves up to
`min(count, FUTEX_REQUEUE_BATCH)` waiters from `from` to `to` in FIFO
order — fresh `seq` values, relative order preserved — and returns how
many moved; the caller loops while the return equals the cap. Requeued
threads keep their deadlines: expiry finds them at the new address
through the rewritten key. Both buckets are held together, taken in
ascending bucket index (one lock when they collide) — the first and
only path in the system to hold two bucket locks.

Two details make the move safe rather than a mere relink. First,
`wait_key` is mutable here while expiry and reap use it to find the
node, so each waiter moves under a third `wake_state` value: requeue
CASes `PARKED -> MOVING` (a `CLAIMED` waiter's node is left in place
for its winner, which is already spinning on one of the buckets requeue
holds), extracts the node, rewrites its key and the TCB's `wait_key`,
re-links it in the destination tree, and releases `MOVING -> PARKED`.
Expiry and reap treat an observed `MOVING` as spin-briefly-and-retry,
and after any party wins a CAS on `PARKED` the keys are guaranteed
stable. Wakers never observe `MOVING` at all: they reach nodes only
through the buckets requeue is holding. Second, the `MOVING` window
must be strictly pointer work — a timer IRQ spins on it, so it must
never contain an allocation (which would put `g_allocator_lock` inside
an IRQ spin window). Requeue therefore allocates nothing at all: the
vendored LLRB grows an extract/insert-existing-node pair (`_remove`
today frees its node and `_insert` allocates one, so relinking needs
the two ~15-line variants), making the move a true relink. That also
removes the failure mode: requeue has no `SYSERR_NOMEM` and no partial
batch indistinguishable from an exhausted queue — the only short
return is the batch cap, which the caller already loops on.

The condvar broadcast is `FUTEX_WAKE(cond, 1)` then
`FUTEX_REQUEUE(cond, mutex, seq, UINT64_MAX)`; the wake-one is not
fused in, because the compare guard already covers the race and the
argument budget is spent.

The three calls are the whole surface. `SYS_VM_SHARE`, `SYS_VM_FREE`,
and the kernel-scheme establishment protocol are unchanged.

## 3. Wait queues

**The bucket table.** A static array `g_futex[1024]` of `{ spinlock;
llrb_futex *waiters; }`, indexed by `hash(addr & ~3ull)`. Same pattern
and same rank as `g_stripes` (below the per-process list locks), but a
plain `spinlock` rather than an `svclock`: no futex path is ever held
across `as_flush`, and the timeout path (§6) takes a bucket from
interrupt context, where a shootdown-servicing lock is forbidden.

**Each bucket is an ordered tree, not a chain.** Reuse the existing
`<llrb/llrb.h>` template exactly as `timer_queue.h` does — an owning
map, a compound key, a plain pointer value:

```c
struct futex_key { uint64_t address; uint64_t seq; };

#define LLRB_NAME  futex
#define LLRB_KEY   struct futex_key
#define LLRB_VALUE thread_ptr
#include <llrb/llrb.h>
```

`seq` comes from one global monotonic counter, taken at park. It is the
tiebreaker the template requires (`_insert` rejects duplicate keys) and
it makes waiters on one address **contiguous and in FIFO order** in the
tree, so a wake is a lower-bound seek to `(addr, 0)` followed by
ascending iteration while the address matches. Removal invalidates the
template's iterators, so the wake loop re-seeks the lower bound of
`(addr, last_seq + 1)` after each removal — O(log n) per woken waiter,
bounded by the batch. Ordering by `tid` would
work as a tiebreaker but would wake in tid order, which lets a
high-tid thread starve; a park sequence costs the same and gives
fairness.

Search is O(log n) in the bucket population regardless of how addresses
distribute, so nothing needs to bound chain length or degrade gracefully
when it grows.

Three consequences to weigh:

- **A node is allocated per park.** `thread.h` states that "waiter growth
  never allocates kernel memory"; that invariant is amended here. What it
  protects against is user-driven unbounded kernel growth, which remains
  bounded: the node count cannot exceed the thread count, and every one
  of those threads already carries a TCB with a CPUID-sized XSAVE area.
  The allocation is on the contended-lock path, which already costs two
  syscalls and two context switches.
- **`FUTEX_WAIT` can therefore fail with `SYSERR_NOMEM`.** `_insert`
  returns false on allocation failure rather than panicking, unlike this
  kernel's other allocation sites, which assert. libc's fallback is
  `sys_yield()` and retry — under memory exhaustion a contended lock
  degrades to spinning. That is a real failure mode with no good
  userspace answer; it is accepted because the alternative shapes trade
  it for hand-written pointer surgery of comparable risk.
- **The template needs a lower-bound iterator.** It offers `_iter_begin`
  (from the minimum) and `_get` (exact match); seeking the first node
  `>= key` is ~15 lines added to the vendored template. Without it a
  wake iterates from the tree minimum, which is O(n).

TCB fields carry only what removal and arbitration need:

```c
struct thread {
  ...
  struct futex_key wait_key;    // keyed node removal by wakers, expiry, reap
  uint64_t deadline;            // timer-tree key half; 0 = not armed (§6)
  uint32_t deadline_cpu;        // whose deadline tree holds the armed entry (§6)
  _Atomic uint32_t wake_state;  // IDLE / PARKED / MOVING / CLAIMED (§2, §6)
};
```

§9 records a variant that allocates one node per address rather than per
waiter, should node churn measure.

**Ring waiters are ordinary tree waiters.** A thread parked on a kernel
channel's `cq_count` is in the same bucket, in the same tree, as any
other waiter, and there is no per-topology waiter storage anywhere. Any
word of a ring block may be waited on; the kernel only ever *wakes*
`cq_count` itself.

The consequence to accept deliberately: an IRQ handler that wakes a ring
waiter (§4) performs a tree removal, which ends in `free()`, which takes
`g_allocator_lock` — the single global spinlock covering every `malloc`
and `free` in the kernel plus `buddy_page_free`. **This is legal but
contended.** Legal because it is a plain `spinlock`, cli-first, so a
holder on the same CPU cannot be interrupted, and because it is never
held across `as_flush` — that is what disqualifies `g_umem` from
interrupt context ([irq-design.md](irq-design.md) §4), and it does not
apply here. Contended because every device interrupt on every CPU now
reaches one global lock: NVMe completions, timer expiries, network RX.

That is a real scalability cost, accepted for now and recorded in §9 as
the first thing to fix. The alternative — a private FIFO hanging off
`struct ring`, with `cq_count` as the only waitable word on such a block
— buys the interrupt path an allocation-free wake at the price of a
second waiter representation, a topology branch in the park path, and an
ABI restriction, all to work around a property of the allocator rather
than anything intrinsic to address-keyed waiting.

The seek and the rebalance are not part of the cost worth worrying
about: a few levels of descent and a rotation are a few hundred
nanoseconds, and the tree bounds them at O(log n) however waiters
distribute.

**A parked thread holds no reference to its block.** The block may be
freed under it (§5); nothing in the kernel needs to find that thread
again by block, so the TCB stores only the tree key. The address a
parked thread names may therefore be recycled into another process's
allocation, and a `FUTEX_WAKE` from that process can reach it. That is a
spurious wake carrying no data; the woken thread rechecks a predicate in
memory it no longer has a view of and takes a loud pristine fault, which
is the same outcome as any other use of a revoked block.

**Park protocol.** Take the bucket; walk the caller's AS and require
`PAGE_U` at `addr`; load the word; compare; insert; arm any deadline
(§6); drop the bucket; `uthread_park_blocked()`. A wake between the
last look at the word and the park cannot be lost because the waker
takes the same bucket — the same argument that makes
`channel_block_wait` race-free today, with the bucket in place of the
stripe.

The view check must sit *inside* the bucket hold, and the placement is
load-bearing twice over. Authorization: without a view check,
compare-and-park is a 32-bit equality oracle over every word in the
SASOS — probe `expected` values against an address you were never
granted, and `SYSERR_AGAIN` versus a park reads the memory.
Atomicity: `channel_block_wait` today closes the check-to-load window
with the process list lock (the comment in `umem.c` says so
explicitly); here the IRQs-off bucket hold is the fence instead. A
revocation's frames return to the buddy only after its shootdown
completes, and the shootdown cannot complete while this CPU sits in an
IRQs-off section — so the `PAGE_U` walk plus the word load inside the
hold cannot race a revoke-and-recycle. The worst case is reading a
just-revoked frame the caller legitimately viewed a moment earlier,
indistinguishable from loading right before the revoke.

That fence covers the *frame*; it does not yet cover the walk itself.
The software walk chases child-table pointers, and `as_flag`'s
overwrite and merge paths call `free_table` during mutation — freed
page-table pages return to the buddy immediately, not after the
shootdown — so a lock-free walker can dereference a recycled table
page. The fix is to make table pages die like user frames: `as_flag`
defers freed sub-trees into the existing post-flush release batch
(`struct umem_release`), so no page-table page is recycled before the
shootdown completes and the IRQs-off fence covers every pointer the
walk touches. That change belongs to
[memory-design.md](memory-design.md) and is a prerequisite of step 1
(§10); until it lands, the interim discipline is the one
`channel_block_wait` uses today — hold `p->ulock` across the check and
the load, ahead of the bucket in lock order.

With no topology branch, **park resolves no ublock at all** and touches
no umem structure: the wait path drops out of the umem lock hierarchy
entirely, and only `FUTEX_WAKE` still takes a list lock (to dispatch
kernel-ring drains). This needs one change elsewhere:
`umem_protect(prot == 0)` currently sets `view = 0`, making a guarded
page unreadable to the kernel too, so the load above could fault. Leave
guards kernel-readable — the same mapping revocation already restores —
and the load is unconditionally safe. That is strictly more consistent
with pristinity, not less: a user guard becomes exactly a revoked page.

**What disappears.** `classify_side`, both structural waiter slots, the
private FIFO and its four `ublock` fields, `side_waiter`,
`channel_block_private_idle`, the one-waiter-per-side `SYSERR_EXIST`,
and the peer-liveness fail-fast check (§5 covers it). Two capabilities
appear as a side effect: **any number of threads may wait on one channel
or ring**, and **blocks with two or more sharers become waitable**,
which the old design refused because the kernel would have had to push
notifications to an unbounded list. A count-capped wake is that bound.

## 4. Kernel-initiated wakes

The kernel wakes from exactly three contexts. The constraint is that
only one of them is a user thread that can be told "call me again".

| site | context | rule |
|---|---|---|
| `FUTEX_WAKE`, revocation, free, reap | a user thread's syscall | cap per call, caller re-enters |
| `ring_post_locked` from `schemes/irq.c`, `schemes/timer.c`, `iommu.c` | IRQ handler | **wake exactly one** |
| `channel_thread_complete_locked` | scheduler stack, post-deschedule | **wake exactly one**, chain the rest |

**Pop under the lock, unblock outside it.** `thread_unblock` spins on
the target's `on_cpu` until its context save completes. Today's
`wake_slot` does that spin holding the block's stripe, which is
tolerable for one thread; a batch wake must not hold a bucket across up
to `FUTEX_WAKE_BATCH` such spins. Follow managarm: detach the batch into
a local list under the bucket lock, drop the lock, then unblock each.
The bucket hold becomes a few pointer writes. The waker CASes each
thread `PARKED -> CLAIMED` as it detaches, skipping any it loses; after
dropping the bucket it removes each won thread's deadline entry (§6),
writes the syscall result into the saved frame, and unblocks — or, for
a thread of a since-dead process, leaves the TCB detached for reap
(§6). Result
delivery and cleanup are the waker's job by construction:
`uthread_park_blocked` never returns, so a woken thread runs no kernel
exit code — it resumes in ring 3 directly from its saved frame.

**IRQ context.** One CQE means one worker is needed, so a post wakes one
waiter on the ring's `cq_count`. This is the same bound as today's
single slot and it is also the semantically correct count — waking N
threads for one completion is a stampede, not a feature. The handler
does a seek, a removal, and a `free()`, all bounded: O(log n) for the
tree work, and `g_allocator_lock` is contended but never held for long
and never held across a shootdown (§3).

The rules from [irq-design.md](irq-design.md) §4 carry over, with the
lock list extended: the handler may take stripes, futex buckets,
`g_allocator_lock`, and the scheduler lock, and must never take
`g_umem`. All of those are plain cli-first spinlocks whose holders do
bounded work with interrupts off; `g_umem` is excluded because it is a
shootdown-servicing svclock and a handler spinning on it can be the very
shootdown target its holder waits for.

**Scheduler-stack context.** `pthread_join` works by registering a
`completion_event` word at `SYS_THREAD_SPAWN`; the kernel pins the block
(`thread_pins`) and release-stores `GDOS_THREAD_COMPLETE` only after the
thread is fully descheduled, because the joiner frees the dead thread's
stack and TLS the instant it observes the word. The only context that
runs after a thread has fully descheduled is `scheduler_loop` on the
per-CPU bootstrap stack — not a user thread, no trap frame, nothing to
return `SYSERR_AGAIN` to. Today's code concedes the point by waking one
batch of 16 and abandoning any remainder.

The correct version is `wake(&completion, 1)`, made sufficient by the
word being a **durable, monotonic, one-shot** transition published under
the bucket lock *before* the wake: a thread that parks afterward sees
`GDOS_THREAD_COMPLETE` and never parks at all, and a thread parked
before is in the FIFO. For the multi-joiner case — undefined behaviour
in POSIX, but the raw ABI permits it — userspace **chain-wakes**: a
waiter that observes the transition calls `FUTEX_WAKE(&completion, 1)`
before returning. One extra syscall per waiter, and zero kernel state.
Chain-waking is the general answer for any context that has no
continuation driver.

## 5. Revocation and teardown

**The kernel does not wake waiters when a block is revoked.** A thread
parked on an address in a block that goes away is not notified; its
recovery is the deadline it parked with (§6), after which its next
`FUTEX_WAIT` on that address returns `SYSERR_INVAL` because the view is
gone. `SYSERR_DEAD` is not among `FUTEX_WAIT`'s results.

Linux never faces this: a shared futex key holds a reference to the
page, so no other process can pull the memory out from under a waiter.
That option is closed here — the ublock model is that an owner may
revoke and sharers must expect it, and pinning would reintroduce exactly
the hostage problem [ipc-process-design.md](ipc-process-design.md) §1
rejects. The remaining choice is between the kernel enumerating waiters
at revoke time and userspace driving the teardown, and the latter is
what the rest of the system already does.

### Orderly teardown is userspace-driven

Revocation is not something that happens spontaneously to a live
process's memory. It happens because the owner asked, and an owner that
is tearing down a channel knows what it is tearing down:

```
write a close sentinel into the protocol words
    -> FUTEX_WAKE them
    -> each peer observes the sentinel, stops touching the
       block, and VM_DROPSHAREs its view — that is the ack
    -> VM_FREE(base) once the sharers drain
       (VM_UNSHARE(base, pid) coerces a peer that never acks)
```

The order matters: **wake before revoke, never after.** An earlier
draft revoked first and had the peer "wake, recheck, and fail its
re-park" — but the recheck is a userspace load of a word the peer no
longer has a view of, and a pristine fault is fatal: revoke-then-wake
kills the cooperating peer it meant to release. Waking into a close
sentinel first makes the peer's own `SYS_VM_DROPSHARE` the ack, so the
owner never needs to know when the recheck finished — `VM_FREE`'s
attached-sharer check is the drain gate. `SYS_VM_UNSHARE(base, pid)` —
the owner's per-edge revoke, pid mandatory; the sharer's own drop,
today's syscall 9, renames to `SYS_VM_DROPSHARE(base)`, splitting the
one verb of [memory-design.md](memory-design.md) §5 into two named by
actor — is thereby the coercion path: a peer that never acks within the
owner's patience is revoked, and its later touch of the block is the
ordinary revocation death, on the non-cooperating party by
construction. Both sides already agree on which words they park on —
role inference is gone, so the protocol names them — and no enumeration
of waiters is needed anywhere.

`SYS_VM_FREE` therefore becomes a **single bounded transaction**. It
fails while anything remains attached — sharers, DMA pins, capability
grants, reflected-fault waiters — rather than driving their removal
itself. The enumeration syscalls that let userspace find those
attachments are recorded as TODOs in the documents that own them
(memory-design for sharers and blocks, pci/iommu-design for DMA pins,
capability-design for grants). Free keeps `SYSERR_AGAIN` reserved in its
ABI and callers keep their retry loops, but no path returns it.

### Disorderly teardown is the parent's job

When a process is killed, three populations are left behind, and they
are handled by three different parties:

- **Its own threads.** Culled at their next kernel entry or dispatch;
  blocked TCBs are freed in batches by reap.
- **Mutexes it held.** `SYS_SET_ROBUST_LIST` registers the Linux-shaped
  per-thread list; the parent walks it during reap, sets
  `FUTEX_OWNER_DIED` in each word, and wakes one waiter. Doing this in
  userspace avoids Linux's faulting reads from the exit path and lets
  the parent implement recovery policy. Its coverage is exactly Linux's:
  the walk only acts on words in the owner-TID encoding
  (`(uval & FUTEX_TID_MASK) == dying tid`), so it reaches robust mutexes
  and nothing else — not condvar sequences, not ring `cq_count`, not
  semaphores. Two further limits, both below Linux and accepted for v1:
  the walk reaches only words the parent can *view* — the child's owned
  blocks (claimed via `VM_MOVE`) and blocks the parent itself shares;
  a robust word in a third party's block that the child merely shared
  in is unreachable and is not recovered. And it runs at process death
  only: a voluntary `pthread_exit` walks the exiting thread's own list
  in libc before `SYS_THREAD_EXIT`, but an involuntary single-thread
  death (a ring-3 fault kills one thread while its process lives)
  walks nothing.
- **Its memory.** The parent claims each block with the upward
  `VM_MOVE`, then frees it with the ordinary flow above — the same
  cleanup routine it would run for its own memory. `SYS_PROC_REAP` does
  **not** free memory (§below).

Peers parked on words in the dead process's memory are covered by the
first bullet if they were mutex owners, and otherwise by their own
deadline — which exists only if they parked with one. Deadline recovery
is a promise the *parker* makes: libc should default cross-process
waits to a finite deadline, and a deadline-0 park on another process's
liveness relies on protocol, not on the kernel (§8). A process that
wants to react faster runs a monitor thread on its tree or shares
channel and wakes its own workers.

A timed-out waiter must also assume nothing about its view: touching
the word from userspace before revalidating risks exactly the pristine
fault the deadline saved it from. The cheap revalidation is re-entering
`FUTEX_WAIT` — a revoked view returns `SYSERR_INVAL` from the kernel's
own check, and that error, not a userspace load, is the recovery
signal.

### `SYS_PROC_REAP` narrows to process and thread state

Reap handles only what userspace cannot: the child's TCBs (including
removing their futex tree nodes), its share edges, its address space,
its registry entry, and its process struct. Blocks the zombie *owned*
must be claimed by the parent with `VM_MOVE` before reap can finish;
reap reports how many remain rather than freeing them.

This keeps the one continuation the system genuinely needs in the one
place it cannot be avoided, and it puts memory reclamation — with all
its sharer, pin, and grant entanglement — on the same userspace-driven
path as every other free. It also makes accounting honest: a parent that
takes over a dead child's memory is charged for it, and a parent that
declines to reclaim leaks only within its own subtree.

### Thread reap must remove futex nodes

This one is not optional and is unrelated to notification. A parked
thread's tree node holds a `thread_ptr`; if the TCB is freed while the
node is still in a bucket, that is a use-after-free, and worse under
recycling — the block returns to the buddy, is reallocated to another
process at the same address, and that process's `FUTEX_WAKE` finds a
stale node. The batched blocked-TCB reap path claims each thread with
the `PARKED -> CLAIMED` CAS, removes its futex node by `wait_key` and
its deadline entry, and then frees the TCB. A thread observed `CLAIMED`
or `MOVING` belongs to someone else for the moment: reap skips it and
reports progress through its existing retry contract rather than
freeing (§6).

The useful framing: **thread death, not block death, carries the
memory-safety obligation.** Block death only ever carried a
notification, and notification is what moves to userspace.

## 6. Timeouts

The timer scheme is deleted alongside this work, leaving the per-CPU
deadline machinery the dispatch quantum requires anyway
([timer-design.md](timer-design.md)). Parked threads are its only
entries: key `(absolute deadline, tid)`, value `struct thread *`,
inserted at park on the parking CPU and removed by whichever path wins
the thread — no entry outlives its wait (below). Each CPU's LAPIC one-shot arms for `min(quantum, earliest
deadline)`, and arming is always local — the scheme's remote-reprogram
IPI goes with it.

Three hazards meet here: a lock inversion (park needs the timer lock
while working under the bucket; expiry holds the timer lock and wants
the bucket to unlink), an arming race (a deadline that exists before
`PARKED` is published, or before the thread is committed to parking,
can fire against a thread that will never deschedule), and cleanup (a
woken thread runs **no kernel exit code** — `uthread_park_blocked`
never returns, and a resumed thread goes straight to ring 3 from its
saved frame, so nothing downstream of a wake can remove anything). The
resolution is one ordering on the park side and one rule on the wake
side.

**Park publishes everything under the bucket:**

- take the bucket; view-check, load, and compare the word (§3) — a
  mismatch exits here with nothing armed and nothing inserted;
- insert the futex node;
- if a deadline was given: take the local CPU's timer lock — nested
  inside the bucket, which is safe because expiry drops the timer lock
  before it ever takes a bucket — insert the deadline entry, record
  `deadline` and `deadline_cpu` in the TCB, publish
  `wake_state = PARKED`, program the LAPIC if the minimum moved, and
  drop the timer lock. If the deadline node cannot be allocated,
  remove the futex node and return `SYSERR_NOMEM` — the bucket is
  still held, so no waker can have observed the partial wait;
- otherwise just publish `PARKED`;
- drop the bucket and park unconditionally.

When the bucket drops, both nodes and the `PARKED` state exist
together or not at all: there is no window in which a party can win
the thread yet fail to find its entries.

**Whoever wins the thread claims it, cleans up everything, then
disposes of it:**

```
win the CAS  PARKED -> CLAIMED   (the claim is a lifetime pin)
remove the futex node            (bucket lock, by wait_key)
remove the deadline entry        (deadline_cpu's timer lock, if armed)
clear deadline to 0
live process:  write the result into the saved frame; thread_unblock()
dead process:  leave the TCB detached — reap frees it
reap itself:   free the TCB directly; no unblock
```

The claim is what makes the out-of-bucket window safe. Today's reap
frees any TCB it finds off-CPU and `THREAD_BLOCKED` — which is exactly
the state of a won thread while its winner holds the TCB pointer and
walks the timer tree — so `CLAIMED` must be a reap-visible pin: reap
skips a `CLAIMED` or `MOVING` thread and reports progress through its
existing retry contract, and only a reap that itself wins
`PARKED -> CLAIMED` may free. Off-CPU-and-blocked alone is no longer a
licence to free. Symmetrically, a waker that claimed a thread of a
since-dead process must not make it runnable; it leaves the TCB
detached for reap. Clearing `deadline` keeps "zero means unarmed" true
for the thread's next untimed wait.

Each cleanup step is idempotent against the others' partial progress:
expiry has already popped its own entry by the time it wins, so a
winner's keyed removal finding nothing is normal — the per-CPU timer
lock serializes the two. An observed `MOVING` (§2's requeue state)
means spin briefly and retry the CAS; after a won CAS both keys are
stable. A wake landing between the bucket drop and the deschedule is
carried by `thread_unblock`'s `on_cpu` spin — which is why the park
after a successful publish is unconditional.

**No deadline entry outlives its wait.** Nothing stale ever fires, a
re-park can never receive a previous park's timeout, and reap never
leaves an entry pointing at a freed TCB. A winner on another CPU may
remove an entry from the arming CPU's tree, but removal never makes a
tree minimum earlier, so it never reprograms a LAPIC and never needs
an IPI; a one-shot firing for an already-removed entry finds nothing
due and rearms harmlessly.

The deadline is absolute monotonic nanoseconds in the `SYS_GETTIME`
domain, so `clock_nanosleep(TIMER_ABSTIME)` and glibc-style absolute
condvar timeouts map directly. Relative and `CLOCK_REALTIME` conversions
are userspace's, as the timer doc already states — with the recorded
caveat that a converted absolute `CLOCK_REALTIME` wait does not track
realtime clock steps made while it sleeps; matching Linux there needs a
kernel realtime clock, which does not exist yet.

With the timer rings gone there is no second entry type in the per-CPU
tree and no tagged value — the tree holds parked threads and nothing
else.

## 7. Accounting

Deleting (kernel):

| what | ~lines |
|---|---|
| `classify_side`, `wake_slot`, `wake_thread`, FIFO push/pop/batch | 81 |
| `channel_block_wait`, `channel_block_doorbell` | 112 |
| `channel_block_free_step`, `channel_block_private_idle` | 21 |
| waiter half of `channel_block_torn` and its assert | 14 |
| `side_waiter`, prototypes, contract comments | 25 |
| four `ublock` waiter fields + `freeing` + comments | 14 |
| `freeing` ripples across `umem.c`, `rc == 1` in `syscall.c` | 45 |
| the block-freeing steps of `SYS_PROC_REAP` (§5) | 40 |
| **total** | **~350** |

Adding: bucket table and template instantiation (~15), the template's
lower-bound iterator (~15), park (~40), wake (~40), requeue with its
`MOVING` arbitration and the template's extract/insert-node pair (~80),
futex-node removal at thread reap (~10), the `VM_UNSHARE` owner-revoke
verb and `VM_DROPSHARE` rename (~20), `SYS_SET_ROBUST_LIST`
registration (~10), deadline arming, expiry, and the CAS arbitration
(~120), TCB fields — **~350**.

**Net ≈ 0 in lines.** About 200 of the added lines are timeouts and requeue —
features the old mechanism did not have at all; the replacement of the
wait/wake mechanism proper is roughly −200. The kernel gets one
mechanism where it had three, gains timed waits, and sheds the
resumable-free protocol entirely. The timer scheme's deletion — roughly
another −350 — is accounted in [timer-design.md](timer-design.md), not
here.

Userspace shrinks: the `SYSERR_AGAIN` doorbell retry loop in `pthread.c`
disappears, and the waiters-bit convention makes an uncontended
`pthread_mutex_unlock` cost zero syscalls where it currently always
calls the doorbell.

## 8. Userspace

**Rings.** One implementation for both kinds of far end, which is the
point:

```
submit and wait:  FUTEX_WAIT(&hdr->cq_count, seen, ring_base, 0)
consume:          while ((cqe = kring_peek_cqe(r))) ...
ack:              store cq_head; FUTEX_WAKE(ring_base, 1)
```

The ack's count is 1, not 0: a kernel channel ignores the count (the
call is the doorbell either way), but against a user peer a count of 0
wakes nobody — and one loop must serve both far ends.

For a kernel channel `wake_addr` drains the SQ; for a user peer it wakes
whoever is parked on the peer's word. The kernel-ring loop is
self-terminating without a re-ring: every drained SQE posts a
completion, so `cq_count` always moves when work was done, and the ring
can never park with undrained work. When the CQ is full the drain stalls
but the caller has CQEs to consume, so it does not park either.

Deadline 0 is safe against a kernel channel — the far end cannot die.
Against a user peer the loop should default to a finite deadline:
§5's recovery story *is* the deadline, and a deadline-0 park on another
process's liveness is a promise the protocol must keep, not the kernel.

The one userspace-visible loss is role inference: two user peers must
now agree on which word each side parks on, rather than the kernel
inferring "the other side". That is the price of deleting
`classify_side`, and it is what lifts the one-waiter-per-side limit, so
a server can finally put a thread pool on one channel.

**pthreads.** Every primitive in `pthread.c` becomes the textbook
futex version, keyed on its own word instead of on the containing
allocation. This fixes the design's worst current defect: today
`wake_private_block` loops on `SYSERR_AGAIN` until the *entire block's*
FIFO is drained, so one `pthread_mutex_unlock` in a heap block holding a
hundred parked threads costs seven syscalls and a hundred wakeups, of
which ninety-nine recheck an unrelated predicate and re-park.
`pthread_cond_broadcast` becomes a real broadcast rather than an
over-wake that happens to work. `PTHREAD_PROCESS_SHARED` works by
construction — cross-process futexes are the same code as private ones —
and stops being `ENOTSUP`.

**Linux emulation.** `FUTEX_WAIT`/`FUTEX_WAKE`/`FUTEX_CMP_REQUEUE` map
directly, including the returned counts. `FUTEX_WAIT_BITSET` is ~8
lines if wanted (a mask in the TCB, matched on wake) but is unnecessary
if absolute deadlines are native, which they are. Still missing,
deliberately: `futex_waitv` (userspace multiplexes into one word, per
the ipc doc's §2 stance), PI futexes, and kernel-exit-path robust
handling — the robust-list registration and the parent-side walk are
§5's.

## 9. Deferred, with recipes

- **A per-CPU slab allocator for the wait nodes** — the first thing to
  fix, and the reason §3 accepts a global lock on the interrupt path.
  Every device interrupt that wakes a ring waiter currently frees a node
  through `g_allocator_lock`, the one heap lock for the whole kernel.
  The requirement is specifically a **remote-free** path: a node is
  allocated on the parking thread's CPU and freed on whichever CPU takes
  the interrupt, so per-CPU magazines alone do not help — the free must
  push onto the owning slab's list with a CAS rather than a lock, the
  way SLUB's remote free does. Cheap to adopt once it exists:
  `<llrb/llrb.h>` already takes per-instantiation `LLRB_MALLOC` /
  `LLRB_FREE` overrides, so the futex tree can be pointed at a slab
  without touching the template or the pid-registry and timer trees.
- **One tree node per address instead of per waiter.** The §3 variant:
  key by address alone, value a FIFO head threaded through one intrusive
  TCB pointer.
  Removes allocation from all but the first waiter on an address and
  needs only exact-match lookups, at the cost of a value-mutation
  accessor on the template. Revisit if park/unpark node churn measures.
- **Wake batching per target CPU.** `scheduler_enqueue` round-robins, so
  a broadcast of `FUTEX_WAKE_BATCH` threads can send that many
  reschedule IPIs. Grouping the batch by target CPU and sending one IPI
  each is a scheduler change, not a futex change.
- **`SYS_THREAD_JOIN(tid)`.** Thread completion (§4) is the one wake in
  the system that is *naturally thread-directed*: one dying thread, one
  joiner registered at spawn. Expressing it as an address wait is what
  forces `completion_block`/`completion_event` on the TCB,
  `thread_pins`, its four checks across `umem.c`, and the 22-line
  validation block in `proc_sys_thread_spawn`. A join syscall that parks
  the caller and is unparked by the scheduler-stack path would delete
  ~60 lines and one `ublock` field, and needs no pinned memory because
  there is no word to write. Independent of everything above; worth
  doing on its own merits.
- **`KEV_SHARE_REVOKED` on scheme `-1`.** Today `KEV_SHARE` fires when
  an edge is created and nothing fires when one is destroyed, so a
  process learns that a share it depended on vanished only by faulting
  on it. An event would let a monitor thread wake its own workers
  immediately rather than leaving them to their deadlines, and would
  cover every wait convention rather than only mutex ownership. ~20
  lines in the revoke path; the awkward part is that the edge carrying
  the level state is the thing being destroyed, so a full CQ has nowhere
  to defer to.
- **A userspace parking lot.** If the kernel table is ever judged too
  expensive for process-private primitives, Redox's own TODO and
  WebKit's `ParkingLot` describe the fallback: a per-process hash table
  over a thread-directed park, with the kernel futex retained for
  process-shared objects. This is a strictly additive userspace change
  and does not constrain anything here.
- **Capability-named wait objects.** If per-primitive kernel objects
  ever become preferable to an address-keyed table — the seL4 trade
  (§1) — the recipe is: a new ublock-free object type in the grant tree
  of [capability-design.md](capability-design.md), a three-state
  Idle/Waiting/Active machine with an OR-coalescing badge, and an
  intrusive TCB queue. The badge semantics are already familiar here:
  they are the same level-state shape as un-notified share edges and the
  IRQ `raised`/`acked` pair. Nothing in this document would survive that
  change, which is the point of recording it now rather than
  discovering the option later.

## 10. Suggested implementation order

1. **The bucket table and the two syscalls**, private blocks only —
   the template instantiation and its lower-bound iterator, park, wake,
   the `wake_state` machine and deadlines (§6), futex-node removal at
   thread reap, no `wake_addr`. Port `pthread.c` to it, including
   `pthread_cond_timedwait` and `pthread_mutex_timedlock`, and keep
   `tests.c` green. This step carries the paging prerequisite from §3 —
   page-table pages reclaimed after the shootdown — or ships with the
   interim `p->ulock` discipline until that lands.
2. **The timer scheme's deletion** ([timer-design.md](timer-design.md)):
   add `SYS_GETTIME`; the scheme surface, `kring_timer.h`, and the
   timer helpers in `kring.c` and `tests.c` go; the per-CPU deadline
   tree narrows to parked threads only. Step 1 gives every timer
   consumer its replacement, so the window where ring entries and
   parked threads share the tree ends here.
3. **Rings onto the tree**, deleting both structural waiter slots and
   the owner slot. `ring_post_locked` wakes one; the handler's lock set
   grows by `g_allocator_lock` (§4). Delete `classify_side` and the
   peer-liveness check.
4. **Teardown** (§5): delete `freeing`, the resumable free, and the
   waiter-drain steps of reap; add owner-revoke `VM_UNSHARE(base, pid)`
   (the sharer drop renames to `VM_DROPSHARE`); narrow
   `SYS_PROC_REAP` and make the parent claim blocks with `VM_MOVE`.
   Tests: a client parked on a server's ring across the server's death
   and across an explicit revoke; `VM_FREE` refusing while a sharer
   remains; a full parent-driven reclaim of a killed child's memory.
5. **`wake_addr` fusion** and the `kring.c` rewrite to one loop for
   both far ends.
6. **`FUTEX_CMP_REQUEUE`** — the two-bucket move with the `MOVING`
   arbitration (§2), and `pthread_cond_broadcast` switched to
   wake-one-plus-requeue. Deliberately last among the mechanism steps:
   the base wait/wake/deadline machine is implemented and tested
   without `MOVING` in it, and the finished system still never ships
   the broadcast stampede.
7. **`SYS_SET_ROBUST_LIST`** plus the parent-side walk in libc, and
   `PTHREAD_MUTEX_ROBUST` — shipped documented as below POSIX for the
   involuntary death of a single thread in a surviving process (§5);
   closing that gap needs a thread-death hook that does not exist yet.
8. **`nanosleep` and the rest of the timed libc surface**, once the
   ring loop is settled.
