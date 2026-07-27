# Park design: thread-directed waiting

Status: **planned 2026-07-26.** An explicit alternative to
[futex-design.md](futex-design.md), not a companion to it: both replace
`SYS_BLOCK_WAIT` / `SYS_BLOCK_DOORBELL`, both reuse syscall numbers 11
and 12, and exactly one of them should be built. They agree on the
problem, on the design law they must obey — bounded non-blocking kernel
work, registrations for interest, events for results, no kernel threads,
no deferred kernel work — and on most of the teardown story. They
disagree on one thing: **what the kernel names when it blocks and wakes
a thread.** Futex names an address and keeps the wait queue in the
kernel. Park names a thread and keeps the wait queue in userspace. §11
is the head-to-head.

Planned implementation map: `abi/gdosabi/syscall.h` (the two syscall
numbers, reused in place, plus `SYSERR_TIMEDOUT` and `SYSERR_INTR`),
`abi/gdosabi/park.h` (new: the park descriptor and `park_control`),
`kernel/src/park.{c,h}` (new: park, unpark, the permit state machine),
`kernel/src/channel.c` (waiter slots move from `ublock` to `struct
ring`; `classify_side`, `channel_block_wait`, `channel_block_doorbell`
delete), `kernel/src/umem.c` (`freeing` and the resumable free delete;
`VM_UNSHARE` becomes the owner-revoke verb `(base, pid)`, the sharer's
drop renaming to `VM_DROPSHARE`), `kernel/src/process.c` (reap narrows
to process and thread state), `kernel/src/thread.h` (park state,
`park_control`, `parked_on`), `kernel/src/schemes/timer.c` (the scheme
surface deletes — [timer-design.md](timer-design.md) — leaving the
per-CPU deadline tree, repurposed for parked threads),
`packages/gdoslib-dev/kring.c` and
`packages/gdos-libc-dev/source/pthread.c` (the two consumers, plus a new
wait-queue library).

## 0. Decisions log

- **The kernel names threads; userspace names conditions.** A wait queue
  is a mapping from "the thing being waited for" to "the threads waiting
  for it". Futex puts that mapping in the kernel, keyed by address. Here
  it lives in userspace as an intrusive list per synchronization object,
  and the kernel holds only the per-thread half: is this thread parked,
  and does it have a permit. The kernel gains no per-object state at all.
- **`unpark` is a permit, not an edge.** It must be recordable against a
  thread that has not parked yet, because the waiter protocol is
  *enqueue → recheck → park* and a waker can pop the node inside that
  window. A non-sticky unpark loses that wake and deadlocks. This is the
  same stickiness that makes the interrupt bit race-free (§3).
- **The permit is authenticated by a per-park secret.** Naked
  `unpark(tid)` has a dense, guessable namespace and most threads are
  parked most of the time, so blind spraying is a viable denial of
  service. The waiter publishes a 64-bit `hint` before enqueueing; the
  waker must present it. Since the hint is published *into the shared
  region the queue lives in*, the effective authority is "you have a
  view of the object" — the same gate futex gets from view membership,
  expressed as data rather than as a kernel-checked relation. That is
  the idiom [capability-design.md](capability-design.md) already
  committed to.
- **The hint must be published before the enqueue, not passed to park.**
  This is forced, and it is the least obvious constraint in the design;
  §3 derives it. It costs one registered per-thread word.
- **Kernel-initiated wakes need no naming at all.** A ring endpoint
  holds `struct thread *` directly, so an IRQ handler waking a ring
  waiter is a list pop and an unblock — no hash, no lookup, no
  allocation, and therefore not `g_allocator_lock`. This is the one
  place the two designs differ in *kind* rather than in taste:
  [futex-design.md §3](futex-design.md) accepts a global heap lock on
  every device interrupt and records a per-CPU slab allocator as the fix;
  here the cost never arises.
- **Nothing is allocated on the wait path.** `thread.h`'s existing
  invariant — "waiter growth never allocates kernel memory" — survives
  unamended, and `SYSERR_NOMEM` is not among park's results.
- **`park` fuses an optional ring-arm and an optional unpark**, for the
  same reason `FUTEX_WAIT` fuses a wake: submit-then-wait on a kernel
  ring, submit-then-wait on a user ring, and unlock-then-wait for a
  condvar must each be one syscall. The argument budget (four registers)
  cannot carry it, so park takes a versioned descriptor by pointer, as
  `SYS_THREAD_SPAWN` already does for its start descriptor.
- **Interrupt is the same mechanism with a second permit bit.**
  `SYS_UNPARK` with `UNPARK_INTERRUPT` sets a sticky interrupt flag and
  makes the park return `SYSERR_INTR`. This is what `EINTR` and deferred
  `pthread_cancel` need, and it is why the "who may poke a thread"
  authority question must be answered whichever design wins — futex does
  not avoid it, it only leaves it to a second mechanism.
- **The interrupt bit and register access are separate capabilities.**
  Interrupt is spurious-wake-grade: worst case a victim's park loop
  spins. Register access is total control. Bundling them means whatever
  delivers `SIGTERM` also holds arbitrary code execution over every
  process it can signal.
- **Timeouts are in the park call, from the start**, for the reasons
  [futex-design.md §0](futex-design.md) gives, unchanged: the deadline
  is the park state machine, not a feature bolted onto it, and every
  `poll`/`epoll`/`select`/`nanosleep`/`*_timedwait` routes through it.
- **The cost is a userspace wait-queue library.** Roughly 300 lines of
  intrusive lock-free list code in libc, of the kind that is famously
  easy to get subtly wrong. This design does not reduce total lines; it
  moves them out of the kernel and removes one class of kernel
  contention. §7 is honest about that.
- **Linux `futex(2)` stops being directly emulable.** Programs that call
  it through libc are unaffected; programs that call it raw need a
  userspace hash table over park (§9). Futex has no equivalent gap.

## 1. Precedents

Surveyed from the four reference trees (`references/{managarm,redoxos,
seL4,minix}`). **None of the four is a park/unpark system**, which is the
first thing to say plainly: the two microkernels with kernel-scheduled
user threads and shared-memory rings both chose futexes, and this
document is arguing against their choice on grounds specific to
govindos. The supporting evidence is therefore partly in-tree TODOs and
partly out-of-tree systems.

- **Redox** (`src/syscall/futex.rs`) implements futexes over a global
  `Mutex<HashMap<PhysicalAddress, Vec<FutexEntry>>>`, and carries this
  TODO verbatim: *"futex is probably the best API for process-shared
  POSIX synchronization primitives, [but] a local hash table and
  wait-for-thread kernel APIs (e.g. lwp_park/lwp_unpark) could be a
  simpler replacement"* for process-**private** futexes. That is the
  strongest in-tree statement of this design's thesis, from a kernel
  that shipped the other one. Redox already has the thread-directed half
  built for a different purpose: `ContextVerb::Interrupt`
  (`src/scheme/proc.rs`) is `guard.unblock()` on a named context, used
  by userspace signal delivery, and there are only five `unblock()`
  callers in the whole kernel. Their factoring — a shared kernel↔user
  page (`Sigcontrol`) holding pending state, checked by the kernel under
  the context lock at every block site — is the direct ancestor of §3's
  `park_control`.
- **seL4** (`src/object/notification.c`) is closer to this design than
  to futex in one respect and further in another. Closer: the waiter
  queue is an **intrusive TCB queue** (`ntfnQueue_head/tail`) with no
  allocation, and the wake target is found without hashing anything.
  Further: the queue hangs off a capability-named kernel *object*, one
  per synchronization primitive, which is exactly the per-primitive
  kernel state both this design and futex exist to avoid. Its
  three-state Idle/Waiting/Active machine with an OR-coalescing badge is
  the level-state shape already familiar from un-notified share edges and
  the IRQ `raised`/`acked` pair. §9 records what adopting it would mean;
  the recipe is identical for either design.
- **MINIX 3** (`minix/lib/libmthread/`) has **no kernel synchronization
  primitive at all**, and its user-level `mthread_mutex_lock` appends the
  current thread to the mutex's own FIFO before suspending. That FIFO is
  precisely the userspace wait queue §8 needs, and it is in-tree as a
  reference for the shape (not for the mechanism — MINIX threads are
  cooperative and never enter the kernel, which is unavailable to us:
  preemptive SMP kernel-scheduled threads are a committed feature and
  `pthread.c` already spawns real ones). The `_lwp_park` files under
  `lib/libc/compat` are inherited NetBSD userland, not a MINIX
  implementation.
- **Managarm** (`kernel/thor/generic/thor-internal/futex.hpp`) is the
  strongest counter-evidence: the closest system to govindos chose
  address keying, with a `FutexRealm` of `hash_map<FutexIdentity, Slot>`
  and a standing TODO to make that table scalable. Two details transfer
  anyway. Its waiter `Node`s live **in the waiter's own frame** rather
  than in allocated entries — under park that property is total, because
  the node is in userspace. And its `wake()` pops the batch under the
  lock and raises completions after dropping it, which §4 adopts for the
  same reason. Managarm also demonstrates the §9 signal story: the posix
  subsystem delivers signals by rewriting a target thread's registers
  through a capability (`helLoadRegisters`/`helStoreRegisters`,
  `SignalContext::raiseContext`), with **no kernel signal support at
  all**.
- **NetBSD `_lwp_park`/`_lwp_unpark`, Solaris `lwp_park`, Java's
  `LockSupport`, WebKit's `ParkingLot`** (not in tree, from general
  knowledge, and worth verifying before committing): all four build full
  mutex/condvar/semaphore stacks on thread-directed parking with the
  queues in userspace, so the shape is proven at scale. My recollection
  is that Solaris and NetBSD both retained *additional* kernel-assisted
  paths for process-shared and robust mutexes rather than doing
  everything over park, which — if true — points at exactly the place
  §5 and §9 flag as this design's soft spot. Two details of NetBSD's
  interface are worth naming because they map onto decisions here.
  `_lwp_unpark_all(targets, ntargets, hint)` takes an **array** of
  lwpids, which is `UNPARK_MANY` (§2) and is the reason fan-out is not a
  point of difference between the two designs. And `_lwp_park`'s hint is
  used as a sleep-queue hash key, not as an authorization secret — §3's
  hint is a different thing wearing the same name, and the distinction
  matters (see the SASOS note there).

## 2. Syscalls

```
SYS_UNPARK 11 // (tid, hint, flags)     -> 0 | SYSERR_*
SYS_PARK   12 // (desc_ptr, desc_len)   -> 0 | SYSERR_*
```

**`SYS_UNPARK(a, b, flags)`** — never parks, never allocates. Three flag
bits, with different meanings and different authority:

- `UNPARK_PERMIT` (default). `a` is a tid, `b` is a hint. Requires `b`
  to equal the target's currently published hint (§3). Deposits a
  permit; if the target is already parked, unblocks it. Returns 0
  whether or not the target was parked — a permit consumed later is the
  same outcome. `SYSERR_PERM` on hint mismatch, `SYSERR_INVAL` on an
  unknown or dead tid.
- `UNPARK_MANY`. `a` is a pointer to an array of `{uint64_t tid;
  uint64_t hint;}` pairs and `b` is its length. Wakes up to
  `min(b, UNPARK_BATCH)` of them and **returns how many entries were
  consumed**, so the caller advances and loops — the same cap-and-loop
  contract `FUTEX_WAKE`'s `count` has, for the same reason: a user
  thread requesting fan-out can be returned to and re-enter, so the
  bound costs nothing but syscalls. Entries whose hint does not match
  are skipped, not fatal; a broadcast racing a timeout is normal.
- `UNPARK_INTERRUPT`. Ignores the hint; requires the interrupt
  capability for the target's process (§10 for what that means before
  the capability plumbing exists — intra-process is unrestricted, since
  a thread can already corrupt its own process's memory). Sets the
  sticky interrupt bit; a parked target is unblocked with `SYSERR_INTR`
  and a target that parks later returns `SYSERR_INTR` immediately.

Each wake is O(1) plus a tid lookup. The lookup is the one structure
park needs that futex does not, and it never runs in interrupt context
(§4).

**`SYS_PARK(desc_ptr, desc_len)`** — may park. The descriptor is
versioned by length, following `SYS_THREAD_SPAWN`'s start descriptor:

```c
struct park_desc {
  uint64_t deadline;      // absolute SYS_GETTIME ns; 0 = none
  uint64_t arm_base;      // ring block base to arm and drain; 0 = none
  uint32_t arm_expected;  // cq_count sampled before the caller decided to park
  uint32_t flags;
  uint64_t unpark_tid;    // fused unpark target; 0 = none
  uint64_t unpark_hint;
};
```

Steps, in order:

1. If `unpark_tid != 0`, perform `UNPARK_PERMIT(unpark_tid,
   unpark_hint)` and release everything it touched. Failure is reported
   only through the caller's own subsequent state; park does not abort
   on it, because the peer may have died between the queue read and the
   call.
2. If `arm_base != 0`, resolve it to a kernel channel the caller has a
   view of. Drain and execute its SQ in the caller's context under the
   existing `RING_SQ_BATCH`/`RING_SQ_CHUNK` bounds and run the scheme's
   replay. Then, under the endpoint lock, compare `cq_count` against
   `arm_expected`; if it differs, return `SYSERR_AGAIN` without parking.
   Otherwise link the caller's TCB into the endpoint's waiter list.
3. Consume a pending permit or interrupt if one is present, undoing
   step 2's link, and return.
4. Otherwise park until unparked, interrupted, or the deadline passes.

| result | meaning | Linux |
|---|---|---|
| 0 | unparked (permit consumed) | 0 |
| `SYSERR_AGAIN` | `cq_count` had already moved; caller did not park | `EAGAIN` |
| `SYSERR_TIMEDOUT` | the deadline passed while parked | `ETIMEDOUT` |
| `SYSERR_INTR` | interrupted | `EINTR` |
| `SYSERR_INVAL` | bad descriptor, or no view of `arm_base` | `EINVAL` |

`SYSERR_TIMEDOUT` (`-9`) and `SYSERR_INTR` (`-10`) are new. `SYSERR_INTR`
must be distinct from every other result or libc's retry loops will
swallow interrupts, which is the whole point of having it.

There is no `SYSERR_NOMEM`: nothing is allocated. There is no
`SYSERR_DEAD`: a parked thread is not notified when memory it was
waiting on is revoked (§5).

A park with no permit, no arm, and no deadline is a permanent park, woken
only by unpark or interrupt. That is the correct primitive for
`pthread_cond_wait`, and it is why the interrupt bit is not optional.

## 3. Park state

**Nothing per synchronization object exists in the kernel.** The whole
of park's kernel state is per-thread:

```c
struct thread {
  ...
  struct park_control *park_control; // registered at spawn; §3.1
  _Atomic uint32_t     park_state;   // PARKED / PERMIT / INTERRUPT / WOKEN
  struct ring         *parked_on;    // endpoint waiter list back-pointer
  struct thread       *wait_prev, *wait_next;  // already present
};
```

`wait_prev`/`wait_next` already exist in `thread.h` for exactly this
purpose, with the comment that motivates the design: *"a thread can be
blocked in at most one syscall, so its TCB supplies all queue storage;
waiter growth never allocates kernel memory."* Under futex that
invariant is amended away; here it holds, and it holds for ring waiters
too.

### 3.1 Why the hint is published, not passed

The waiter protocol is *publish hint → enqueue node → recheck predicate
→ park*, and a waker can pop the node and call unpark at any point after
the enqueue, including before the park. So the kernel must be able to
record a permit against a not-yet-parked thread. Three ways to make that
authenticated, two of which fail:

- **Permit carries the hint, park checks it on arrival.** An attacker's
  `unpark(T, garbage)` overwrites a legitimate pending permit, and the
  legitimate wake is lost. That is a deadlock, strictly worse than the
  denial of service the hint was introduced to prevent.
- **Permit slot is write-once until consumed.** An attacker fills it
  first and blocks legitimate permits. Same deadlock.
- **The kernel knows the thread's expected hint before the park, and
  rejects mismatches without touching any state.** This is the only
  shape that works, and it requires the hint to be readable by the
  kernel during the pre-park window.

Hence `park_control`: a small structure in the thread's own memory whose
address is registered once, in `SYS_THREAD_SPAWN`'s start descriptor, and
validated once with `user_range_ok`.

```c
struct park_control {
  _Atomic uint64_t hint;  // 0 = not accepting permits
};
```

Userspace stores its per-park secret there with a plain store before
enqueueing, and zeroes it after waking. The kernel reads it on every
`UNPARK_PERMIT`. Ordering is carried by the queue itself: the hint store
happens-before the node publication, and a waker that read the node has
therefore seen the hint.

**No pin is required.** If the process frees the block containing its own
`park_control`, the kernel reads recycled memory — in a SASOS that is a
garbage value, not a fault, and the consequence is that the hint check
fails and the thread does not get woken. That is the process breaking
itself, with no memory-safety consequence for the kernel, and it is the
same reasoning [futex-design.md §3](futex-design.md) uses for a parked
thread whose address is recycled. This avoids the `thread_pins` /
`completion_block` machinery that the join word currently needs.

The alternative, if registered user memory is judged too clever, is a
`SYS_PARK_ARM(hint)` syscall on the contended path only — one extra
syscall per blocking acquisition, zero new kernel-read-user-memory
surface. It is strictly simpler and strictly slower.

### 3.2 The hint is a secret, not an address

NetBSD's `_lwp_park` hint is the address of the wait object, used to
pick a sleep queue. **That shape is worthless here.** Govindos is a
SASOS: addresses are global, stable, and as guessable as tids, so an
address-valued hint authenticates nothing. The hint must be a random
value obtainable only by reading the shared region — which is what makes
the authority equivalent to futex's view gate.

Two consequences:

- **Per-park, not per-thread.** A stable per-thread token, once leaked in
  any context, would grant wakes in every future unrelated park. A fresh
  hint per park scopes the authority to the thing it was granted for,
  and has the side benefit that a *stale* unpark from a previous wait is
  rejected rather than delivered as a spurious wake — which futex cannot
  do.
- **64 bits, not a capability token.** The threat is denial of service
  only: a successful forgery produces a spurious wake, and every park
  loops and rechecks its predicate regardless. 64 unguessable bits are
  far past sufficient. The 32-byte MAC'd grant references of
  [capability-design.md](capability-design.md) are for authorities where
  forgery has correctness impact; using them here would cost the
  register budget for nothing.

The hint is not consulted for kernel-initiated wakes (§4) or for
`UNPARK_INTERRUPT`, which has its own authority.

### 3.3 Ring waiters live on the endpoint

`struct ring` gains an intrusive waiter list head, threaded through the
TCB links above, and the TCB gains `parked_on` so that thread reap can
unlink in O(1) without a key. This is the current design's waiter slot
relocated from the `ublock` to the endpoint, which is what dissolves the
problems [futex-design.md §0](futex-design.md) attributes to ublock
anchoring: a kernel endpoint has exactly one side, so `classify_side`
and the owner/sharer slot pair go away; the endpoint is a kernel object
with its own lifetime, so wait identity does not change when the block's
share topology does; and there is no `freeing` protocol because there is
no waiter state in the block at all.

De-anchoring from the ublock is what those deletions require. Address
keying is one way to achieve it; moving the list to the endpoint is
another, and it keeps the wake path allocation-free.

Two capabilities appear as a side effect, the same two futex gains: **any
number of threads may wait on one ring**, lifting today's
one-structural-waiter-per-side cap, and **blocks with two or more
sharers become waitable**. The bound that makes the first safe is the
wake-exactly-one rule of §4.

### 3.4 The park state machine

`park_state` is the arbitration point for four parties that can race to
end one park: `UNPARK_PERMIT`, `UNPARK_INTERRUPT`, deadline expiry, and
thread reap. All four CAS it; the winner performs the unblock and any
list removal, the losers discard. This is the same arbitration
[futex-design.md §6](futex-design.md) specifies for `wake_state`, and it
is required for the same reason — the deadline path runs in interrupt
context and must not take a lock the park path holds.

Permits and interrupts are sticky bits in the same word, so a park that
begins with either set consumes it and returns without ever linking
itself anywhere.

## 4. Kernel-initiated wakes

The kernel wakes from exactly three contexts. Only one of them is a user
thread that can be told "call me again", which is what bounds fan-out;
and in the other two — the ones that matter for the interrupt-path
argument — **the kernel already holds a `struct thread *`**, so nothing
is named, looked up, or hashed:

| site | context | rule |
|---|---|---|
| `SYS_UNPARK`, `SYS_PARK`'s fused unpark | a user thread's syscall | cap per call, caller re-enters |
| `ring_post_locked` from `schemes/irq.c`, `schemes/timer.c`, `iommu.c` | IRQ handler | **wake exactly one**, list pop |
| `channel_thread_complete_locked` | scheduler stack, post-deschedule | **wake exactly one**, chain the rest |

**Pop under the lock, unblock outside it.** `thread_unblock` spins on the
target's `on_cpu` until its context save completes; holding the endpoint
lock across that spin is tolerable for one thread and not for a batch.
Follow managarm: detach under the lock, unblock after dropping it.

**IRQ context.** One CQE means one worker is needed, so a post pops one
waiter from the endpoint's list and unblocks it. Waking N threads for one
completion is a stampede, not a feature. The handler's work is a pointer
write and an unblock — **no tree seek, no allocation, no
`g_allocator_lock`**. The lock list from
[irq-design.md](irq-design.md) §4 is therefore unchanged: stripes, the
endpoint lock, and the scheduler lock, never `g_umem`. This is the
concrete payoff of thread-directed naming, and it is the argument that
does not reduce to taste.

**Scheduler-stack context.** `pthread_join` today registers a
`completion_event` word at spawn, and the kernel publishes it only after
the dying thread is fully descheduled — a context with no trap frame and
nothing to return `SYSERR_AGAIN` to. Under park the joiner's TCB pointer
is stored directly on the dying thread, so the wake is `unpark` with the
pointer in hand and one target. Multiple joiners are undefined in POSIX
but permitted by the raw ABI; userspace **chain-unparks**, the observing
waiter unparking the next. Zero kernel state per additional joiner. §9
records that this whole path collapses into `SYS_THREAD_JOIN(tid)`,
which park makes natural rather than merely possible.

## 5. Revocation and teardown

This section is substantially the same as
[futex-design.md §5](futex-design.md), because it follows from the
ublock model rather than from the wait mechanism. The differences are
marked.

**The kernel does not wake waiters when a block is revoked.** A thread
parked while a block goes away is not notified; it recovers via its
deadline, or not at all if it parked without one. Pinning the memory to
protect waiters would reintroduce the hostage problem
[ipc-process-design.md](ipc-process-design.md) §1 rejects.

**Orderly teardown is userspace-driven.** Revocation happens because an
owner asked, and an owner tearing down a channel knows what it is
tearing down:

```
write a close sentinel -> unpark the peers named in the protocol
                       -> peers VM_DROPSHARE — their ack
                       -> VM_FREE(base) once the sharers drain
                          (VM_UNSHARE(base, pid) coerces a peer
                           that never acks)
```

`SYS_VM_UNSHARE(base, pid)` is the owner's per-edge revoke (the
sharer's own drop renames to `SYS_VM_DROPSHARE` —
[memory-design.md](memory-design.md) §5). `SYS_VM_FREE`
becomes a single bounded transaction that fails while anything remains
attached rather than driving removal itself, and `freeing` and the
resumable free delete.

**Difference from futex:** the owner wakes peers by tid, not by address.
It must therefore know their tids and hints, which means the protocol
carries them — which it already does, since that is how the queue works.
The recipe above unparks before any revoke for the same reason futex's
does — a revoked peer can neither be read (its queue nodes) nor safely
recheck anything — and `VM_UNSHARE` is only the coercion path for a
peer that never acks.

**Disorderly teardown is the parent's job**, with three populations
handled by three parties: the dead process's own threads (culled at next
entry, TCBs reaped in batches), the mutexes it held
(`SYS_SET_ROBUST_LIST`, walked by the parent at reap), and its memory
(claimed with the upward `VM_MOVE`, then freed normally). `SYS_PROC_REAP`
narrows to process and thread state and does not free memory.

**Difference from futex, and this one is a real cost.** Linux-shaped
robust recovery sets `FUTEX_OWNER_DIED` in the word and wakes one
waiter. Waking one waiter here means reading the object's userspace
queue to find the head's tid and hint, and then unparking a thread that
may belong to a *third* process. So the parent needs both a view of the
dead child's memory (it has one, via `VM_MOVE`) **and** cross-process
unpark authority over an unrelated process's threads. Under futex the
second requirement does not exist: a view of the memory is the whole
authority. This is the sharpest place where thread naming costs
something that address naming does not, and it is why §10 sequences the
capability work before robust mutexes rather than after.

**Thread reap must unlink parked threads.** A TCB in an endpoint's
waiter list that is freed is a use-after-free. The batched blocked-TCB
reap path unlinks via `parked_on` — O(1), no key, no tree. As futex puts
it: thread death, not block death, carries the memory-safety obligation.

## 6. Timeouts

Identical in mechanism to [futex-design.md §6](futex-design.md). Each
CPU owns a deadline LLRB and programs its LAPIC one-shot for
`min(quantum, earliest deadline)`
([timer-design.md](timer-design.md)). Parked threads become deadline
entries keyed `(absolute deadline, tid)` with value `struct thread *`,
inserted at park and removed on wake.

The ordering and arbitration are [futex-design.md §6](futex-design.md)'s,
adapted to the endpoint list: everything — the link, the deadline entry
(timer lock nested inside the endpoint lock), and the published park
state — exists before the endpoint lock drops, and the thread then
parks unconditionally (a mismatch exits earlier with nothing armed and
nothing linked). Whichever path wins the park — unparker, expiry, or
reap — claims the thread, and the claim is a reap-visible lifetime pin:
the winner removes the node and the deadline entry before unblocking
(or, for a dead process's thread, marks it reapable), and reap frees
only threads it claimed itself or that a winner marked reapable; a
woken thread runs no kernel exit code, so cleanup is always the
winner's. No entry outlives its park: nothing stale ever fires, a
re-park cannot receive an earlier park's timeout, and reap never meets
a freed TCB.

One simplification over futex: because a park has at most one list to be
unlinked from, and the back-pointer is in the TCB, expiry does not need
a key to find anything.

The deadline is absolute monotonic nanoseconds in the `SYS_GETTIME`
domain, so `clock_nanosleep(TIMER_ABSTIME)` and glibc-style absolute
condvar timeouts map directly. Relative and `CLOCK_REALTIME` conversions
are userspace's.

This section is unaffected by the timer scheme's deletion
([timer-design.md](timer-design.md)); it depends only on the per-CPU
deadline machinery, which the dispatch quantum requires regardless.

## 7. Accounting

Deleting (kernel), largely shared with futex since both remove the same
`ublock`-anchored machinery:

| what | ~lines |
|---|---|
| `classify_side`, the two structural slots, private FIFO push/pop/batch | 81 |
| `channel_block_wait`, `channel_block_doorbell` | 112 |
| `channel_block_free_step`, `channel_block_private_idle` | 21 |
| waiter half of `channel_block_torn` and its assert | 14 |
| `side_waiter`, prototypes, contract comments | 25 |
| four `ublock` waiter fields + `freeing` + comments | 14 |
| `freeing` ripples across `umem.c`, `rc == 1` in `syscall.c` | 45 |
| the block-freeing steps of `SYS_PROC_REAP` (§5) | 40 |
| **total** | **~350** |

Adding: park and unpark including the descriptor read (~70), the
`park_state` machine, deadline arming, expiry, and CAS arbitration
(~120), the endpoint waiter list and its relocation into `struct ring`
(~50), a tid→TCB lookup (~25), `park_control` registration and reads
(~15), unlink at thread reap (~10), the `VM_UNSHARE` owner-revoke verb
and `VM_DROPSHARE` rename (~20),
`SYS_SET_ROBUST_LIST` registration (~10) — **~320.**

**Net ≈ −30 in the kernel**, against futex's ≈ 0 (which includes its
requeue syscall, the `MOVING` arbitration, and the template relink
ops). Nominally ~30 lines better — inside the error bars of both
estimates, and it should not decide anything.

**Userspace grows by ~300 lines** that futex does not need: the wait-queue
library of §8. So the honest summary is that park **does not reduce total
line count** — it moves a wait queue from kernel to userspace, removes
one global lock from every device interrupt, keeps the no-allocation
invariant, and pays for it with lock-free userspace code and a
cross-process authority requirement.

What park avoids that does not show up as lines: a 1024-entry bucket
table, a per-park kernel allocation with a `SYSERR_NOMEM` failure mode
that has no good userspace answer, a lower-bound iterator added to the
vendored LLRB template, and `g_allocator_lock` on the interrupt path
with a per-CPU remote-free slab allocator queued behind it.

## 8. Userspace

**The wait-queue library.** New, and the main new burden. One intrusive
MPSC queue per synchronization object, nodes allocated on the waiters'
own stacks (managarm's property, here in userspace), each node carrying
`{tid, hint, next}`. The acquisition path is:

```
fast:      CAS the state word; done, no syscall
contended: hint = fresh random; store to park_control
           push node onto the object's queue
           recheck the state word           <- closes the race
           SYS_PARK(deadline)
release:   CAS the state word
           if the queue is non-empty, pop and SYS_UNPARK(tid, hint)
```

The recheck is what makes the permit's stickiness necessary and
sufficient. MINIX's `libmthread` mutex FIFO is the in-tree reference for
the structure, minus the lock-freedom its cooperative scheduler let it
skip.

**Broadcast** pops the whole queue, marshals `{tid, hint}` pairs into a
stack array, and calls `UNPARK_MANY` in a loop until the array is
consumed — O(N/`UNPARK_BATCH`) syscalls, matching what `FUTEX_WAKE`'s
own cap-and-loop costs.

**Requeue is free.** Moving waiters from a condvar's queue to the
mutex's without waking them — what keeps `pthread_cond_broadcast` from
stampeding the mutex — is a list splice between two structures this
library already owns: zero syscalls and zero kernel involvement. Under
futex the same operation is `FUTEX_CMP_REQUEUE`, which
[futex-design.md §2](futex-design.md) provides as a third syscall — ~80
lines including its `MOVING` arbitration and template relink ops, and
the first path in the system to hold two bucket locks. Owning the queue costs the library its
complexity and refunds some of it here.

**Rings.** One loop for both kinds of far end:

```
submit and wait:  SYS_PARK(&(struct park_desc){ .arm_base = ring_base,
                                                .arm_expected = seen })
consume:          while ((cqe = kring_peek_cqe(r))) ...
ack:              store cq_head; SYS_PARK(.arm_base = ring_base, ...)
```

For a kernel channel the arm drains the SQ and the compare-and-link is
done under the endpoint lock. For a user peer, `unpark_tid`/`unpark_hint`
carry the handoff and there is no arm. The kernel-ring loop is
self-terminating without a re-ring: every drained SQE posts a completion,
so `cq_count` always moves when work was done.

Two user peers must agree on tids and hints in their protocol rather
than relying on the kernel to infer "the other side". That is the same
loss of role inference futex incurs from deleting `classify_side`, in a
slightly heavier form — a tid and a hint instead of an address.

**pthreads.** Each primitive in `pthread.c` grows a queue head beside its
existing `_Atomic uint32_t` state word. The current implementation
already funnels everything through one wait/wake pair with five call
sites, so the port touches the same surface the futex port would.
`PTHREAD_PROCESS_SHARED` works by construction once cross-process unpark
authority exists, and not before — see §5 and §10.

**Linux emulation.** `nanosleep`, `clock_nanosleep`, all `*_timedwait`
forms, and `poll`/`select`/`epoll_wait` timeouts map onto a park with a
deadline, which is a better fit than futex offers for the `poll` family
(the timeout is the park's, not a separate timer object's). `EINTR` and
deferred `pthread_cancel` map onto `UNPARK_INTERRUPT`.

The gap: **raw `futex(2)` cannot be emulated directly.** Programs using
libc synchronization are unaffected; programs calling the syscall
themselves need a userspace hash table over park — WebKit's `ParkingLot`
is the recipe, and it is what futex-design already contemplates for the
opposite direction. This is a genuine and one-sided cost.

## 9. Deferred, with recipes

- **A userspace parking lot for raw `futex(2)`.** A per-process hash
  table from address to a queue of parked tids, plus the same
  publish/enqueue/recheck/park protocol. Strictly additive; needed only
  if raw-futex programs become a target.
- **`SYS_THREAD_JOIN(tid)`.** Thread completion is naturally
  thread-directed: one dying thread, one joiner registered at spawn.
  Expressing it as a word wait is what forces `completion_block`,
  `completion_event`, `thread_pins`, its four checks across `umem.c`,
  and the 22-line validation block in `proc_sys_thread_spawn`. Under
  park the joiner is already a TCB pointer, so join is a park with no
  descriptor and an unpark from the scheduler stack — ~60 lines and one
  `ublock` field deleted, no pinned memory because there is no word to
  write. Worth doing on its own merits under either design; park makes
  it fall out.
- **Priority inheritance.** Thread naming is the prerequisite: the
  kernel can only boost an owner it can name. Under futex, PI needs
  kernel-visible ownership encoded in the word (Linux's PI futexes);
  here the owner tid is already the thing being passed. Not a v1 goal,
  but the door is wider on this side.
- **Adaptive spinning.** The queue is in userspace, so spin-then-park
  policy is a userspace decision with no kernel involvement and no new
  ABI. Under futex the same is true, but the kernel table means the
  spin threshold cannot see queue depth; here it can.
- **Splitting the interrupt and register capabilities.** §0's decision,
  deferred only in the sense that the capability plumbing lands after
  the mechanism. `UNPARK_INTERRUPT` is spurious-wake-grade;
  `SYS_THREAD_STOP`/`GET_REGS`/`SET_REGS` are total control. They must
  be separate grants in the tree of
  [capability-design.md](capability-design.md) even if one server holds
  both at first.
- **Asynchronous signal delivery.** Interrupt covers every POSIX case
  where the target is blocked, which is every cancellation point. A
  compute-bound thread that makes no syscalls never sees a signal, which
  is what "notifications are doorbells consumed at a wait point"
  ([ipc-process-design.md](ipc-process-design.md) §3) already commits
  to. If that gap ever needs closing, the recipe is managarm's: a
  userspace agent with `STOP`/`SET_REGS` over the target builds the
  frame itself — cheaper here than in managarm because a SASOS lets the
  agent write the target's stack with plain stores. That keeps the
  kernel out of asynchronous user-code delivery entirely, which §3 of
  the ipc doc requires. Redox's alternative — the kernel redirects `RIP`
  at every context switch, stashing the old one in a shared page — is
  ~20 lines and needs no capability, at the cost of the kernel knowing
  a signal-control ABI.
- **Capability-named wait objects.** The seL4 trade (§1): a ublock-free
  object type in the grant tree, a three-state Idle/Waiting/Active
  machine with an OR-coalescing badge, an intrusive TCB queue. Nothing
  in this document survives that change, which is the point of recording
  it. The recipe is identical under either design.
- **Wake batching per target CPU.** `scheduler_enqueue` round-robins, so
  an `UNPARK_MANY` of a full batch can send that many reschedule IPIs.
  Grouping the batch by target CPU is a scheduler change, not a park
  change, and applies equally to futex.

## 10. Suggested implementation order

1. **Park, unpark, and the state machine**, intra-process only —
   `park_control` registration, the permit and interrupt bits, deadlines
   (§6), unlink at thread reap. No arm, no fused unpark. Port
   `pthread.c` and its new queue library to it, including
   `pthread_cond_timedwait` and `pthread_mutex_timedlock`, and keep
   `tests.c` green.
2. **Rings onto the endpoint list**, deleting both structural waiter
   slots, `classify_side`, and the peer-liveness check. `ring_post_locked`
   pops one. Add the descriptor's `arm_base`/`arm_expected` and rewrite
   `kring.c` to one loop for both far ends.
3. **Teardown** (§5): delete `freeing`, the resumable free, and the
   waiter-drain steps of reap; add owner-revoke `VM_UNSHARE(base, pid)`
   (the sharer drop renames to `VM_DROPSHARE`); narrow
   `SYS_PROC_REAP` and make the parent claim blocks with `VM_MOVE`.
   Tests: a client parked on a server's ring across the server's death
   and across an explicit revoke; `VM_FREE` refusing while a sharer
   remains; a full parent-driven reclaim of a killed child's memory.
4. **The fused unpark**, and user↔user ring handoff.
5. **Cross-process unpark authority** in the grant tree, then
   `SYS_SET_ROBUST_LIST` and the parent-side walk, then
   `PTHREAD_PROCESS_SHARED`. This ordering is forced by §5: robust
   recovery needs to unpark a third process's thread.
6. **`nanosleep` and the rest of the timed libc surface**, once the ring
   loop is settled.

Steps 1–4 are strictly intra-process and need no capability work, which
is the same staging futex allows. Step 5 is where the two designs
diverge in cost.

## 11. The choice

Where the two designs actually differ, stated without hedging:

| | futex | park |
|---|---|---|
| kernel names | an address | a thread |
| wait queue | `g_futex[1024]`, LLRB per bucket | userspace, per object |
| kernel state per waiter | a heap-allocated tree node | none (TCB fields) |
| wake path in IRQ context | seek + remove + `free()` → `g_allocator_lock` | list pop |
| allocation failure | `SYSERR_NOMEM` from `FUTEX_WAIT`, no good recovery | cannot occur |
| cross-process authority | a view of the memory, kernel-checked | a 64-bit secret in the memory |
| robust mutex recovery | parent needs a view | parent needs a view **and** cross-process unpark |
| broadcast of N waiters | O(N/batch) syscalls, kernel walks its tree | O(N/batch) syscalls, userspace walks its list |
| requeue (condvar → mutex) | `FUTEX_CMP_REQUEUE`, one syscall | userspace list splice, no syscall |
| raw `futex(2)` emulation | direct | needs a userspace parking lot |
| userspace burden | textbook futex primitives | a lock-free wait-queue library |
| kernel LoC delta | ≈ 0 | ≈ −30 |
| `EINTR` / `pthread_cancel` | needs a second thread-directed mechanism | the same mechanism, one flag |
| priority inheritance later | needs ownership encoded in the word | owner tid already passed |

Three observations that carry the most weight:

1. **The interrupt problem is not optional and does not care which
   design wins.** `EINTR` and `pthread_cancel` require naming a thread
   and poking it. Under park that is one flag on a call that already
   exists; under futex it is a second mechanism with a second authority
   model, sitting beside the first.
2. **The IRQ-path allocator cost is structural, not incremental.**
   futex-design accepts a global heap lock on every device interrupt and
   records the fix as future work requiring a remote-free slab allocator.
   Under park the cost never exists. This is the strongest argument on
   this side.
3. **Fan-out is not a differentiator; restructuring the queue is.**
   Broadcast costs O(N/batch) syscalls under both designs — `FUTEX_WAKE`
   caps at `FUTEX_WAKE_BATCH` and makes the caller loop, `UNPARK_MANY`
   caps identically over an array — because the O(1) rule constrains
   only the two *kernel-initiated* contexts that have no continuation
   driver (§4), never a user thread that can re-enter. Where the queue
   lives does decide **requeue**: moving waiters from a condvar to a
   mutex without waking them is a userspace list splice here (§8), and
   `FUTEX_CMP_REQUEUE` in [futex-design.md §2](futex-design.md) — ~80
   lines of kernel work and the first path to hold two bucket locks.
   Owning the queue is the cost that pays for this.

Against those: park needs cross-process unpark authority before
process-shared and robust mutexes work at all, cannot emulate raw
`futex(2)`, and requires ~300 lines of lock-free userspace code that
futex gets from the kernel for free. Whether the wait queue belongs in
the kernel is the question; everything else follows from it.
