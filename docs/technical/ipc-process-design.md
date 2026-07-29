# IPC & process design: channels, kernel schemes, fault reflection, threads, process trees

Status: **implemented 2026-07-07; planned amendments are specified by
[enumeration-design.md](enumeration-design.md)** — §§1, 2, 4, 5 landed
(channels + schemes -1/-3, process trees, zombies + destruction,
kernel-thread deletion,
`g_as_template` collapse); **§3 fault reflection remains unimplemented**
(its own session, as planned). §7 records where the implementation
deliberately diverges. Companion to [memory-design.md](memory-design.md);
builds directly on its ublock/revoke/pristinity model. Where this
document contradicts details of that one (notably the reaper kthread in
its §7 and the eager teardown walks in its §6), this one is what's
implemented — the reaper kthread is gone.

Implementation map: `channel.c/h` (channels, private events, kernel ring ABI,
schemes, waiter slots), `process.c/h` (tree, parent authority, kill, zombies,
destruction), `umem.c/h` (share edges, revoke authority, VM_MOVE
mechanics, live-pid index), `syscall.c/h` (dispatch; SYSCALL/SYSRET),
`packages/tests/tests.c` (the ring-3 test suite for all of it —
formerly hello.c, which doubled as init until the boot-init design split
the roles; see [boot-init-design.md](boot-init-design.md)).

## 0. Decisions log

- **Channels are shared blocks, not kernel objects.** The kernel's whole
  IPC surface is `SYS_VM_SHARE` plus a futex-shaped doorbell/wait pair
  scoped to a shared block. Ring layout, acknowledgement, flow control —
  userspace protocol between user peers; the kernel never reads or
  learns it. No session object, no handle table: the block's base
  address is the channel name (SASOS: addresses are global names). A
  client's code is bit-identical whether the far end is a user server or
  the kernel (§2).
- **Kernel services are channels, not syscalls.** Redox-scheme-shaped:
  sharing a block to a *negative* id creates a channel whose far end is
  a kernel scheme (§2: share notifications; later: name
  registry, timers, devices). No kernel thread consumes them — commands
  run in the doorbeller's syscall context (the io_uring-enter model) and
  events post in the syscall/IRQ context of whoever caused them. Design
  law replacing "short syscalls must not block": **kernel-channel
  commands are bounded and non-blocking**; long-lived interest is a
  registration in the owning scheme, and results are events.
- **Channel blocks are client-owned.** A client can
  impose costs on a server anyway (request spam, expensive requests);
  servers defend themselves with their own admission policy, in
  which time-per-request can participate. The kernel does not arbitrate.
- **Sharing maps immediately; consent is keeping.** `SYS_VM_SHARE` to a
  user pid maps the block into the target on the spot and posts a
  notification (§2, scheme `-1`); rejection is one `SYS_VM_DROPSHARE` —
  the same bounded work an accept handshake would cost, with no pending
  state to track. The accepted price is the planting caveat (§1): the
  mapping and its bookkeeping exist until the target unshares it. V1
  adds no kernel resource quota.
- **Teardown uses the same per-edge revoke path.** Free, unshare, and
  decline converge on the same umem code. Death itself does no hidden
  revoke work: the dead process's reaper invokes those verbs explicitly.
- **Death is O(1); the dead are zombies until userspace destroys them.**
  Pids are u64, allocated monotonically, never reused. Cross-process
  references are `(pid, base)` pairs checked against a live-pid index,
  not pointers, healed lazily. Reclamation is the parent's teardown loop —
  bounded syscalls, destruction mirroring construction — so there is
  **no kernel work queue** and no deferred kernel work at all (§4).
- **Fault reflection is in** (separate implementation session). The
  debugger requirement alone justifies it; surviving revoked channel
  memory is its first user. Longjmp-shaped: no instruction skipping.
- **No kernel threads.** The only per-CPU kernel contexts are the
  scheduler/idle loops on the bootstrap stacks. Everything currently done
  by kthreads is dissolved (§4). Nothing is deferred inside the kernel,
  either: teardown — the one long-running job — is the parent's
  enumeration/destroy
  loop, so the scheduler/idle loops are pure dispatch.
- **Process trees, parent-driven creation and management, no reparenting
  ever.** Parents build and continue to manage direct children from their
  own resources (block ownership transfer, protection, thread spawn).
  **Killing a process
  logically kills every descendant through immutable parent links.** No
  eager subtree walk is needed. Zero-thread children die with their
  parent by the same rule; a grandparent may clean a descendant only
  through an all-dead path. Daemonization means asking init
  (over a channel) to spawn the image as *its* child — Unix's
  orphan-adoption by pid 1 is deliberately rejected; Fuchsia's job
  trees are the precedent.
- **Resource budgets:** no kernel limit exists in v1.

## 1. Shared-block channels

A channel is a shared block that both sides agree to treat as a ring.
The kernel contributes a consented way to establish the share and a
park/wake pair scoped to the block. Between user peers, everything
inside the block is userspace convention; when the far end is a kernel
scheme (§2), the same two data-plane syscalls apply and the layout is
kernel ABI.

Kernel-side state lives entirely on existing umem structures: an intrusive
FIFO for an owned private event block, two structural waiter slots for a
shared endpoint (one for the owner and one for the sharer), a notified bit per
share edge, and per-scheme endpoint state
for kernel channels (§2). The ublock sharer machinery is the single
source of truth for who participates, and its revoke path is the single
teardown authority. There is no session object, no per-process session
list, no IPC registry. Ring CQ publication uses a lock embedded in the
ring; ring drains and ownership-graph mutations use `g_umem`.

### Syscalls

`SYS_VM_ALLOC`, `SYS_VM_FREE`, `SYS_VM_SIZE`, `SYS_VM_PROTECT`,
`SYS_VM_DROPSHARE` are unchanged from memory-design.md. (Renamed from
`VM_MAP`/`VM_UNMAP`
2026-07-11: in a SASOS everything is already mapped in principle —
these verbs create and destroy blocks, they don't position anything.
2026-07-26: the sharer-side drop renamed from `VM_UNSHARE` to
`VM_DROPSHARE`; `SYS_VM_UNSHARE(base, pid)` is now the owner's per-edge
revoke — memory-design §5.)

- `SYS_VM_SHARE(base, target, prot) -> 0` — `target` is signed.
  - **User pid (> 0): maps immediately.** The whole owned block at
    `base` appears in the target's AS as `prot|PAGE_U` on return, and
    a notification is queued for the target's share channel (§2,
    scheme `-1`). Errors: `SYSERR_INVAL` (not the owner, unknown pid,
    self-share), or `SYSERR_EXIST` (already shared to that pid).
    The share edges themselves are the notification queue — each
    carries a notified bit — so there is no separate queue to size or
    overflow. The target rejects by unsharing; until it does, the
    mapping stands. That is the **planting caveat**, the price of
    having no pending state: a wild pointer in the target that would
    have faulted into pristine memory can land in a sharer-controlled
    block instead — pristinity's loud-fault guarantee is eroded
    exactly there, bounded by the target's `-1` service latency.
    Accepted: addresses are not secrets, and servers defend
    themselves.
  - **Kernel scheme id (< 0): immediate.** The block becomes a kernel
    channel of that scheme, active on return. The kernel initializes
    the header (it is the trusted producer side; §2). Errors:
    `SYSERR_INVAL` (unknown scheme, bad order for the scheme),
    `SYSERR_EXIST` (scheme-specific singleton rule, e.g. one share
    channel per process).

- `SYS_BLOCK_DOORBELL(address) -> 0 | SYSERR_AGAIN` — role-inferred, never
  blocks. `address` may be anywhere in the block; wakeup is block-scoped.
  - Owned block with no sharers or scheme: detaches and wakes at most
    `BLOCK_WAKE_BATCH` (16) threads from its private FIFO. `SYSERR_AGAIN`
    means a full batch was woken and waiters remain, so userspace rings again
    when it requires a broadcast. Userspace supplies durable predicate state;
    unrelated predicates in the same block tolerate spurious wakes by
    rechecking their atomic words.
  - User peer on the far side: wakes the thread parked on the *other*
    side of the block, if any (its `SYS_BLOCK_WAIT` returns 0); success
    and no-op otherwise. Never reads the block.
  - Kernel channel: drains and executes the SQ **in the caller's
    context** — bounded, non-blocking commands only — posting a
    completion CQE per command, then returns 0. Malformed SQEs consume
    a slot and complete with an error status; they never fail the
    doorbell itself. The drain is bounded twice over: at most
    `RING_SQ_BATCH` (1024, compile-time) SQEs per doorbell — enough to
    amortize the syscall — executed in `RING_SQ_CHUNK` (64) slices
    with the control-plane lock dropped and the ring re-resolved
    between slices, so a full SQ can't turn one doorbell into a
    machine-wide stall. Leftovers wait for the next doorbell;
    userspace (`packages/gdoslib-dev/kring.c`) re-rings until the `sq_head` mirror
    catches up to what it published.

- `SYS_BLOCK_WAIT(addr, expected) -> 0` — (may park.) `addr` is any
  4-byte-aligned address inside a block the caller has a view of
  (`SYSERR_INVAL` otherwise). Under the address's futex bucket: load the
  32-bit word at `addr`; if it differs from `expected`, return 0
  immediately; else append to the private FIFO, or park in the caller's
  structural side slot for a shared/kernel endpoint, until a wake: the
  local or peer doorbell or, on a kernel channel, the kernel posting a CQE
  (returns 0); or revocation/identity change (`SYSERR_DEAD`). Parking on a user channel
  whose peer process is no longer live fails `SYSERR_DEAD` up front (a
  live-pid index check, §4) — only threads already parked at the death
  wait for reap-time revocation. Wake and park take the same bucket, so a
  wake between the caller's last look at the word and its park cannot be
  lost — the protocol is verbatim the old
  `ring_wait_user`, generalized to a caller-chosen word. The loaded word is untrusted:
  the kernel only compares it, and a lying peer can only misdirect
  waiters who chose to rendezvous with it. Private blocks admit any number of
  waiters without kernel allocation because each TCB embeds its one intrusive
  wait node. `SYSERR_EXIST` if a shared endpoint's side already has its one
  structural waiter; multi-threaded shared endpoints still shard across
  blocks.

An owned block with zero sharers is the process-private case. A user↔user
channel has exactly one sharer. Both data-plane calls return `SYSERR_INVAL`
on a block with several sharers or when the caller is neither participant.
A kernel channel's far side is the scheme; it has no sharer entry. The
first share changes private-event identity into channel identity and is
rejected with `SYSERR_EXIST` while the private FIFO is nonempty. Userspace
must stop new local waits and drain the existing FIFO before sharing.

### Free is a single transaction; userspace drives what precedes it

`SYS_VM_FREE(base)` is one bounded operation. It **fails** while anything is
still attached to the block — sharers, DMA pins, capability grants,
reflected-fault waiters — rather than driving their removal itself. Detaching
them is userspace's job, done with bounded per-item verbs before the free:

```
write a close sentinel into the protocol words
    -> FUTEX_WAKE them
    -> each peer observes the sentinel and VM_DROPSHAREs — the ack
    -> VM_FREE(base) once the sharers drain
       (VM_UNSHARE(base, pid) coerces a peer that never acks)
```

The kernel never wakes a parked thread because its block was revoked
([futex-design.md](futex-design.md) §5). An owner tearing down a channel knows
which words its peers park on — the protocol names them, since the kernel no
longer infers roles — so it wakes them itself, **before** any revocation: a
peer woken after its view is gone faults fatally on its recheck. Revocation is
reserved for the peer that never acks. A party that wants to survive an
*uncooperative* peer parks with a deadline, and on timeout revalidates by
re-entering the wait (`SYSERR_INVAL` is the revocation signal) rather than
touching the word.

`SYSERR_AGAIN` stays reserved in the ABI and libc `free()` keeps its retry
loop, so a future continuation can be added without touching callers. No path
returns it today. The known candidates are recorded in
[memory-design.md](memory-design.md) §5: adversarially large sharer sets, and
the page-table walk of a large block, which is the one unbounded step no
userspace pre-pass can remove.

**TODO (not yet designed):** the enumeration syscalls this flow assumes —
sharers of a block, blocks of a zombie, DMA pins, grants — with their owning
documents named in memory-design §5.

### Establishment flow

```
server:  VM_ALLOC -> ch; VM_SHARE(ch, -1)      // share channel, once
         BLOCK_WAIT on it; EV_SHARE {pid, base|order} arrives (mapped)
         validate size/peer — VM_DROPSHARE if unwanted —
         else ack CQE in the new channel; DOORBELL(base)
client:  VM_ALLOC -> base; write ring header; VM_SHARE(base, server_pid)
         BLOCK_WAIT(&sh->cq_tail, seen) until the server's ack lands
```

Connect-by-pid is the v1 rendezvous; a name-registry scheme later
changes how the client learns the pid and nothing else. Acceptance,
protocol version, and ring geometry are negotiated in-band: a server
that dislikes a share (wrong size, unwanted peer — the pid in
`EV_SHARE` is what policy keys on) unshares it, which wakes a waiting
client with `SYSERR_DEAD` like any other revocation.

### Ownership and failure modes

Every teardown edge funnels through the revoke path; waking a parked peer
with `SYSERR_DEAD` happens exactly where the view is torn out, so
error-on-park and fault-on-touch cannot disagree.

- **Server unshares:** immediate — the client keeps its own block, and
  a client parked in `SYS_BLOCK_WAIT` wakes `SYSERR_DEAD`. Rejecting an
  unwanted share is this same path, nothing separate.
- **Server dies:** the client keeps its block. A client parking after
  the death fails fast on the liveness check; one already parked wakes
  `SYSERR_DEAD` when the server's reaper drops the shared-in view (§4).
- **Client frees the block:** immediate revocation — the server's view
  is torn out, parked server threads wake `SYSERR_DEAD` / get
  `EV_DEAD`.
- **Client dies:** it becomes a zombie; its blocks stay mapped and
  charged, so the server may even drain the final SQEs. The view is
  torn out when the client's parent reaps: parked server threads wake
  `SYSERR_DEAD` / get `EV_DEAD` then, and a server *touching* the block
  after that takes a fault and recovers via fault reflection (§3).
  Pristinity makes the fault loud and deterministic: revoked pages are
  present-U=0, never a silent read of recycled data. Teardown never
  waits on the far side — no Xen-grant hostage problem.

### Ring convention between user peers: header + arena, offsets not pointers

Userspace protocol — the kernel enforces none of this between user
peers. `ring_shared` sits at offset 0 of the block; the rest of the
2^order block is a data arena. SQE args carry **arena offsets** (each
side bounds-checks `off + len <= arena_size`), never raw pointers — the
server has no view of client memory outside the channel block, by
design. Both sides treat the block as hostile (copy SQE/CQE out before
validating). The word each side sleeps on (`sq_tail` for the server,
`cq_tail` for the client) is part of this convention: `SYS_BLOCK_WAIT`
watches whatever address the protocol designates.

## 2. Kernel channels (schemes)

A kernel channel is a block shared to a negative id. The scheme defines
the command and event vocabulary; the transport, wake rules, and
teardown are identical to user channels. This subsumes the legacy
kernel-worker rings: same interface shape, no consumer thread.
`SYS_RING_CREATE`/`SYS_RING_ENTER`/`SYS_RING_WAIT` and the worker
kthreads are deleted once the first schemes land; hello.c's ring tests
migrate to scheme channels and a user client/server pair.

### Execution model: borrowed context only

Commands execute inside the owner's `SYS_BLOCK_DOORBELL`; events are
posted from the syscall context of whoever caused them (a sharer's
`VM_SHARE`, a peer's doorbell, the revoke path) or from an IRQ handler
(device schemes), through the identity map from any CR3 — SASOS makes
IOCP-style posting a plain store. Nothing here ever blocks or defers;
there is no kernel execution context, which is what lets §4 delete
kernel threads outright.

The kernel keeps authoritative ring indices in its per-channel endpoint
object and never trusts the in-block copies: the block's header fields
are mirrors it writes for the user side's convenience. The
user-published SQ tail is read once per doorbell and bounds-checked
against the kernel's own head (`tail - head <= nslots`, else the
doorbell completes nothing and posts a protocol-error event); SQEs are
copied out before validation, the discipline the legacy worker already
follows. A corrupted mirror harms only the corrupter.

### Kernel ring ABI

The one kernel-defined shared layout. Header at offset 0 (kernel-owned
mirrors: CQ count, SQ head; user-owned: SQ tail), then two arrays of
32-byte entries sized by the block order:

```
SQE {u64 op;   u64 a; u64 b; u64 c}      user -> kernel commands
CQE {u64 type; u64 a; u64 b; u64 status} kernel -> user completions/events
```

Every SQE produces exactly one completion CQE carrying its `status`
(`0` or a `SYSERR_*`); events arrive as CQEs with event types. The CQ
cannot overflow by construction: completions are 1:1 with consumed SQEs
(CQ sized >= SQ), and each scheme bounds its spontaneous events
(per-scheme rules below).

### Scheme `-1`: shares

One per process (`SYSERR_EXIST` on a second). Where incoming shares
announce themselves. No commands — like `-3`, a pure event channel;
rejection is `SYS_VM_DROPSHARE`, an
ordinary syscall, not a scheme op.

| CQE | fields | when |
|---|---|---|
| `EV_SHARE` | sharer pid, `base\|order` | a block was shared in — it is already mapped; also replayed for edges whose notified bit is still clear when the channel is created |

Event bound: one `EV_SHARE` per share edge; a CQ smaller than the number of
edges simply delays replay
until slots free (shares are level-state in the edges — the queue is a
view of it, and nothing is lost if the ring lags). The receiver learns
size and peer identity from the event, never from block contents;
everything *inside* the block is hostile until validated.

A single-purpose server's whole event loop is `BLOCK_WAIT` on this one
channel. A multiplexing runtime uses dedicated structural waiters and
process-private event blocks, or arranges for its servers to publish
already-multiplexed completion queues.

### Process-private events and userspace multiplexing

There is deliberately no scheme `-2`. Kernel wait-groups made a channel
wake forward into another ublock, requiring registration lifetime state,
two-stripe ordering, CQ deduplication, and kernel-side fanout policy. An
arbitrary listener count also makes the borrowed kernel path unbounded.

Instead, an owned unshared ublock is a local event object. One thread parks
on a userspace sequence word with `BLOCK_WAIT`; another updates the durable
queue/sequence and calls `BLOCK_DOORBELL` on the same block. There remains
one parked thread per block, so runtimes shard local event blocks or use a
small number of structural waiter threads.

High-fan-in services multiplex before crossing protection domains: for
example, a network stack posts `{connection-id, buffer}` completions for
many connections into a per-worker channel and rings it once per batch.
Userspace runtimes build epoll-shaped subscription graphs, ready queues,
and coroutine scheduling above these sharded channels. A dedicated waiter
per unrelated service channel can forward into process-private queues when
the producers cannot share one completion stream.

### Scheme `-3`: tree

One per process (`SYSERR_EXIST`). Where the process-tree events of §5
arrive — the SIGCHLD slot of the signal decomposition (§3), delivered
as a message on a channel like everything else.

| CQE | fields | when |
|---|---|---|
| `EV_CHILD_DEAD` | pid | a direct child dies; also replayed for children already dead when the channel is created (the same level-state rule as `EV_SHARE`) |

No commands in v1. A parent's libc parks here (or registers it in a
group) and runs the reap loop (§4) on each event. Not posted when the
parent is itself dying — the grandparent hears about the parent
instead and reaps the whole subtree.

### Future schemes

The negative-id space is the stand-in for a scheme namespace: a name
registry (replacing connect-by-pid), timers (a doorbell at time T — the
notification-shaped answer to SIGALRM), and device schemes, where the
IRQ handler posts the completion CQE — dummydev becomes the first one,
per the §4 table. Each must obey the design law: bounded non-blocking
commands, registrations for interest, events for results.

## 3. Fault reflection

A ring-3 #PF/#GP with a handler registered is reflected to userspace
instead of killing the thread. First users: server recovery from revoked
channel memory; the debugger (which cements the feature — it needs to
catch bad accesses regardless).

Mechanism (cheap here because a user thread's suspended state is already
just a trap frame):

- `SYS_FAULT_HANDLER_SET(entry)` — per-process.
- Delivery: kernel writes the saved frame + fault address onto the
  **faulting thread's own stack** (validated with `user_range_ok(...,
  write)` first — a bad rsp must not have the kernel scribbling through
  the identity map; bad stack ⇒ kill), rewrites the resume frame to
  `rip = entry, rcx = frame ptr`, irets. Kernel retains no state.
- `SYS_FAULT_RESUME(frame_ptr)` — installs a frame from user memory.
  **Sanitized**: cs/ss forced to user selectors, rflags masked to
  arithmetic bits with IF forced on (no IOPL/TF/RF/NT) — else this is an
  SROP gadget.

Rules and non-features, from the design discussion:

- **Longjmp-shaped.** Resuming the faulting rip refaults (nothing fixed
  the mapping); instruction skipping needs a decoder + emulation —
  never. The registers exist so the handler can redirect to a recovery
  point with context.
- **Nested faults kill.** Per-thread in-delivery flag; fault while set ⇒
  dead.
- **Policy stays in userspace.** The kernel reflects every ring-3 fault;
  the runtime keeps a thread-local "risky region" flag around channel
  touches and re-kills for faults outside it (userspace uaccess
  exception tables). No kernel-side recoverable-range registry.
- **Synchronous only, forever.** No inter-thread signals, no timers via
  this path — that asynchrony is what makes POSIX signals hell. (The
  POSIX-signal decomposition this design commits to: faults reflect,
  notifications are doorbells consumed at a wait point, lifecycle is
  `SYSERR_DEAD`/`EV_DEAD` on the transport, kill is a kernel verb that
  runs no user code.)
- Unpreempted refault loops are a CPU hog like any ring-3 spin; user
  preemption (implemented 2026-07-08, see §7) is the mitigation.
- Pristinity guarantees the failure is *loud*: revoked pages are
  present-U=0, so ring 3 deterministically faults and never silently
  reads recycled data.

Implementation deferred to its own session; the client-dies-mid-request
channel case stays untested until then.

## 4. Thread model: no kernel threads

Kernel threads are for **orphaned work** — asynchronous to any request,
required to wait, attributable to no process. Audit of the current four
users says nothing in this kernel permanently qualifies:

| today | becomes |
|---|---|
| ring worker kthreads | scheme commands in the doorbeller's context (§2) |
| dummydev producer | a device scheme: the IRQ handler posts the CQE; the producer was only simulating an IRQ |
| reaper kthread | zombies + the parent's reap loop (below, §5) |
| boot spawner / hello threads | deleted; tests move to init/userspace |

What remains per CPU: the scheduler/idle loop on the bootstrap stack.
Nothing else in the kernel owns a stack, so the "suspend iff you own
your stack" asymmetry disappears — **every thread is a user thread**;
the only suspension is the parked trap frame. `kthread_spawn`,
`thread_block`, kernel-side `yield`, `wait_result`, `t->stack_top`, and
`STACK_TYPE_KERNEL_TASK` are all deleted.

**Cascade:** kthread-stack guard punches were the *only* post-seal
mutation of `g_as_kernel`. Without them, `g_as_kernel` is boot-static —
**`g_as_template` collapses back into `g_as_kernel`** and process
creation clones the live kernel AS again, safely.

### Teardown: O(1) death, zombies, parent-driven reclamation

There is no kernel work queue. The one job that seemed to need one —
process teardown — is driven from userspace instead, one bounded
syscall at a time: **the chunking mechanism is the syscall boundary
itself** (the seL4 discipline — long kernel work is many short kernel
operations). Destruction mirrors construction: the parent assembled
the child out of bounded syscalls, and it takes it apart the same way.

**Identity: u64 pids, never reused.** Monotonic allocation; 2^63 pids
outlast the hardware, so there is no reuse and therefore no ABA — a
`(pid, base)` pair is forever unambiguous. The sole live-process registry is
an owning left-leaning red-black tree keyed by pid. Lookup, insertion, and
removal are O(log n); allocation-free in-order traversal supports the rare
operations that intentionally visit every extant live address space. Tree
allocation is selected through per-instantiation hooks, ready for a future
slab-backed allocator without coupling that policy to the container.
Lookups happen under the registry lock, which pins the pointer for the
critical section: a process leaves the index before its struct is
freed (the discipline umem's registry already follows).

**References: `(pid, base)` pairs, healed lazily.** No structure
outside a process ever holds a pointer into its bookkeeping. Sharer
entries, share edges, and anything else that names a foreign process
stores its pid; every use revalidates against the index and drops
stale entries on the spot (`SYSERR_DEAD` completions where a command
referenced the dead — e.g. a `-2` `ADD` naming a dead peer's channel).
Consequently a process's ublock metadata lives **by value in one
per-process vec**: nothing external points into it, so the final reap
step frees it wholesale.

**Logical death is O(tree depth), independent of subtree breadth.** Mark
the subtree root directly dead and post `EV_CHILD_DEAD` to its live
parent. A process is effectively dead when it or any immutable ancestor
is directly dead; syscall/interrupt checkpoints, scheduler dispatch,
peer-liveness checks, and live-only pid lookup all use that predicate.
The all-process pid index retains zombies until exact final destruction;
raw teardown lookup and live lookup are separate APIs
([enumeration-design.md](enumeration-design.md) §2). Runnable/running
threads die at dispatch or their next kernel entry. Blocked victims are
not woken merely to be culled; userspace enumerates their tids and
`SYS_THREAD_DESTROY` removes each futex node, drops its completion pin
exactly once, and frees the TCB.

**Zombies hold everything.** Pristinity needs no revocation at death:
revocation guards *recycled* memory, and a zombie's blocks are still
allocated. Sharers' views survive — a server may drain a dead client's last
requests. Parents are responsible for promptly dismantling their dead
subtrees; v1 has no resource quota backstop.

**`SYS_PROC_DESTROY(pid) -> PROC_DESTROY_MORE |
PROC_DESTROY_DONE | SYSERR_AGAIN | SYSERR_EXIST`** — callable on an
exact effective-dead descendant when every process from the target
through the caller's direct child is also effective-dead/non-running.
A live intermediate child is a hard boundary. The call never walks a
subtree; userspace discovers descendants and destroys them post-order.

**Destroy handles the empty process body only. It does not free memory,
views, TCBs, IRQ routes, IOMMU objects, or grants.** The parent uses the
enumeration/coercion ABI to dismantle those resources and frees each
owned block with the ordinary §1 flow. Grants are independent of
process death. `SYS_PROC_DESTROY` reports `SYSERR_EXIST` until the
target's block, view, thread, and child trees are empty. This keeps every
resource entanglement on one userspace-driven path.

One bounded step per call, whichever applies:

1. verify exact-target authority and that resources/children are empty;
2. wait for CPU/AS pins (`SYSERR_AGAIN`) or free K page-table nodes
   (`PROC_DESTROY_MORE`);
3. remove the pid-registry and parent-tree entries, then free the
   process struct — `PROC_DESTROY_DONE`.

Mutexes the dead threads held are recovered by the parent walking their
`SYS_SET_ROBUST_LIST` registrations, in userspace, during this loop.
Threads parked on a zombie's channels are not woken by the kernel at
any point; they recover through their own deadlines, or through a
monitor thread on their tree or shares channel. That latency rides on
prompt parents — a libc default (park on the tree channel, reap on
`EV_CHILD_DEAD`), not a kernel guarantee — accepted.

Nothing is silently removed merely because a process died. Share and
DMA edges, views, blocks, and TCBs remain enumerable until the reaper
explicitly removes them; final `PROC_DESTROY` removes the registry entry
in O(log n).

## 5. Process trees and parent-driven creation

Precedents: seL4 (userspace retype-and-build, no spawn syscall), Fuchsia
(parent maps VMOs into an empty process; jobs form the kill-tree),
Genode (parent-donated quotas), Mach (`task_create` + `vm_map` +
`thread_set_state`). Unix fork/exec is the outlier. SASOS bonus: the
classic userspace-loader pain — computing relocations for the child's AS
through a temporary mapping in the parent's — vanishes, because both
views are the same identity addresses. Transfer is a re-flag; exec is
zero-copy.

### Creation protocol

```
SYS_PROC_CREATE()                        -> pid   // alive: AS clone, no threads
SYS_VM_MOVE(base, pid)                            // ownership transfer along tree edges only (below)
SYS_VM_PROTECT(base, len, prot [, pid])           // parent may set a direct child's views
SYS_THREAD_SPAWN(pid, &start, sizeof start) -> tid // versioned entry/arg/RSP/TLS/completion descriptor
SYS_THREAD_BASES_SET(fsbase, gsbase)        -> 0   // change the current thread's user TLS bases
SYS_GETTID()                                -> tid
SYS_THREAD_EXIT()                           -> never // current thread only
SYS_PROC_EXIT(status)                       -> never // whole process; status reported to parent
SYS_PROC_KILL(pid)                                // own descendant; logical subtree death (O(depth))
SYS_PROC_DESTROY(pid)  -> more | done | again | exist // exact dead descendant through all-dead path (§4)
```

- Pids are the u64 never-reused identifiers of §4; `SYS_PROC_CREATE`
  is where they are minted.
- Parent allocates blocks in its own AS, writes the PE image (loader =
  today's pe.c logic as a userland library; relocs computed against
  final addresses), transfers the image + stack blocks, sets per-section
  W^X on the child's views, and spawns its first thread. The direct parent
  retains those move/protect/spawn powers after the child starts.
- `VM_MOVE` runs only **down** a direct parent→child tree edge. General
  gifting between unrelated or non-adjacent processes stays banned;
  peers *share* (channels; an unwanted share costs its
  target one unshare, while a move changes ownership, which is why only
  moves are tree-restricted). A parent can pre-seed a bootstrap channel
  (to itself or a registry server) before first spawn — the seL4/Xen
  answer to how strangers are initially introduced.
- `SYS_THREAD_SPAWN` accepts self or a live direct child. This supplies
  both in-process multithreading and parent-directed thread creation.
- The kernel consumes only an initial user `stack_pointer` (`8 mod 16` at a
  Win64 function entry); allocation bounds and guard pages belong to the
  userspace loader/thread library. It checks that the initial stack page is
  writable, but does not infer or enforce a stack layout. The versioned start
  descriptor also supplies initial FS/GS bases and an optional private
  completion word. Completion is release-published only after the departing
  thread is off every CPU, so an acquire-observing joiner may immediately
  reclaim that thread's userspace stack and TLS allocation.

### The tree

Every process records its immutable parent and children. Killing a process
marks only the subtree root directly dead; every descendant, including an
as-yet zero-thread child, is immediately dead by ancestry. This answers the
orphaned-child problem
without walking a broad tree. Running threads die at their next kernel
entry (the quantum timer bounds hostile CPU-bound code to one quantum);
runnable threads are culled at dispatch; blocked threads stay off the
runqueue and are reclaimed by bounded `SYS_THREAD_DESTROY` calls.

**Nothing ever reparents.** Death follows tree edges down; cleanup and
destruction proceed post-order; the tree is never restructured. A
grandparent may destroy a dead grandchild only after the intervening
child is dead, and it must name each exact process (§4).
Unix's orphan adoption by pid 1 is deliberately rejected — Fuchsia's
job trees are the precedent. Daemonization is a service, not a tree
operation: ask init over a channel to run the image as *its* child;
init is everyone's ancestor and reaps like any parent (libc's default
loop — park on the `-3` channel, reap on `EV_CHILD_DEAD` — makes the
flaky-parent risk a non-issue for anything linking a sane runtime).
init is the root, loaded by a boot-only kernel loader from the ramdisk
(pe.c demoted to exactly one image); init's death is a panic.

### Resource attribution

Cleanup attribution is structural: blocks have an explicit owner and
`VM_MOVE` transfers that ownership. Quantitative budgets are deferred to the
capability design rather than inferred from a user identity.

## 6. Suggested implementation order

1. **Channels + the shares scheme** (§1, §2 `-1`): immediate-map share
   edges (sharer-charged bookkeeping, notified bits), kernel-channel
   plumbing (endpoint object, borrowed-context SQ processing, kernel
   ring ABI), `EV_SHARE` notifications, per-side waiter slots,
   `BLOCK_DOORBELL`/`BLOCK_WAIT`, `SYSERR_DEAD` wakes in the revoke
   path. Legacy `SYS_RING_*` kept temporarily.
2. **Process trees + lifecycle** (§5, §4): u64 pids + live-pid index,
   direct-parent move/protect/spawn authority, lazy descendant
   kill, zombies + `SYS_PROC_DESTROY`, the `-3` tree scheme; pe.c →
   boot-only init loader. This replaces the reaper kthread's job.
3. **Fault reflection** (§3) — its own session, already agreed.
4. **Kernel-thread deletion** (§4): with teardown already
   parent-driven after step 2, delete the kthread machinery + legacy
   kernel rings, dummydev becomes a device scheme, collapse
   `g_as_template` into `g_as_kernel`.
Steps 1–3 are order-independent among themselves; step 4 needs 2 (the
reap model replaces the reaper kthread) and unlocks the template collapse.

In practice steps 2 and 4 were one changeset: the reaper is itself a
kthread, and kill must enumerate every park site, so the legacy
rings/dummydev (and their kthreads) had to go with it.

## 7. Implementation notes and deliberate divergences (2026-07-07)

- **Timer preemption is implemented (2026-07-08); the timer scheme
  shared it (2026-07-17) and is now planned for deletion
  ([timer-design.md](timer-design.md)).** Each CPU's LAPIC one-shot
  (`VECTOR_TIMER=0xFB`) is armed for the earlier of its absolute 10 ms
  dispatch-quantum deadline and its earliest `KSCHEME_TIMER` deadline. The
  idle path removes the quantum but preserves user timers. A timer-only
  interrupt rearms the remainder of the existing quantum; it never grants
  more CPU time. Quantum expiry from ring 3 is an involuntary
  SYS_YIELD: the handler EOIs, runs the death checkpoint, saves the trap
  frame into the TCB and parks-requeued — the same path as a voluntary
  yield, so nothing downstream can tell the difference. The kernel is
  never preempted: IA32_FMASK masks IF for the whole syscall path, so a
  mid-syscall expiry stays pending until sysret and lands one
  instruction into ring 3 (syscalls are implicit critical sections;
  worst-case preemption latency = longest syscall). A shot landing in
  the scheduler loop's own IF=1 windows is treated as spurious. The
  LAPIC timer and TSC are calibrated once on the BSP against polled PIT
  channel 2. `KTIMER_NOW` converts the TSC in-kernel; absolute one-shot
  events remain durable when their CQ is full. Preemption at arbitrary
  ring-3 instructions is what forced eager extended-state handling. The
  original fixed FXSAVE area was replaced on 2026-07-17 by a dynamically
  CPUID.0D-sized, 64-byte-aligned standard XSAVE area per TCB. Each CPU
  enables the BSP-selected XCR0 policy (x87/SSE, AVX when present, and the
  complete AVX-512 state group when present), and every park/resume eagerly
  XSAVEs/XRSTORs it. FSBASE and GSBASE are saved beside that state and restored
  before iretq; `SYS_THREAD_BASES_SET` lets a PE or ELF CRT establish both.
  This also closed the last reap gap: a CPU-bound thread of a
  killed process now hits its death checkpoint within one quantum, so
  nthreads/AS-drain gates clear in bounded time.
- **SYSCALL/SYSRET replaced `int 0x80`** (requested during
  implementation). User ABI: rax = nr, args in **r10**, rdx, r8, r9;
  rcx/r11 are architecturally clobbered (return rip/rflags). The entry
  stub (interrupts.asm) forges the same `struct trap_frame` the ISR path
  builds and stores r10 into its rcx slot, so the dispatcher and the
  frame-save park/resume machinery are shared verbatim. Kernel stack
  reached via swapgs + `cpu_state.syscall_anchor`; vector 0x80's gate is
  DPL 0 now (a user `int NN` just #GPs and kills the thread).
- **The kernel ring header has a fourth index: user-owned `cq_head`**
  (CQEs consumed). §2's header listed only cq_count/sq_head/sq_tail, but
  CQ-full accounting and level-state replay need the kernel to know what
  the user has consumed; the doorbell doubles as the consumption ack for
  every scheme. Concrete ABI (channel.h): 64-byte header, 32-byte
  SQE/CQE, `nslots = 32 << order` each, SQ at 64, CQ after it. Every SQE
  completes with a CQE echoing `{op, a, b}` + status; event types have
  bit 63 set.
- **`PROC_DESTROY` may initially free the whole AS in one call**, since
  `as_free` is not incremental yet. It is bounded by the target's table
  count after resource teardown. Add `PROC_DESTROY_MORE` page-table
  chunking if page trees become large.
- **TCB disposal is enumerated.** Scheduler-owned threads die at their
  next kernel entry or dispatch; detached `THREAD_BLOCKED` TCBs are
  freed by exact `SYS_THREAD_DESTROY(pid, tid)` calls.
- **All death cascades logically.** Natural death and explicit kill mark
  only the subtree root; immutable ancestor traversal makes every descendant
  immediately dead while post-order reap materializes them incrementally.
- **VM_MOVE**: the receiver's view arrives R|W (parent applies W^X via
  VM_PROTECT-with-pid afterwards); kernel-channel blocks refuse to move
  (a scheme endpoint is owner-bound identity); sharer edges survive the
  move; a receiver's pre-existing shared-in view is subsumed. Any move
  wakes parked waiters SYSERR_DEAD — ownership is channel identity.
- **Sharing changes wait identity at both boundaries.** The first share
  turns a private event block into a channel and requires an empty private
  FIFO; the second makes it ordinary multi-sharer memory. Structural endpoint
  waiters are bounded one per side and wake `SYSERR_DEAD` on either transition.
- **Resource budgets are not wired**: there are no kernel identity
  accounts or per-process limits. Explicit budgets remain future work.
- **Wait-groups were removed (2026-07-12).** Scheme `-2`, its ABI, channel
  registration slots, cross-block forwarding, and two-stripe ranked wake
  path are gone. Process-private ublock events plus sharded server queues are
  the userspace building blocks for multiplexing. Existing scheme ids were
  intentionally not renumbered.
- **SYS_THREAD_SPAWN takes a versioned start descriptor** whose `argument` is
  delivered in the new thread's first argument register — how init hands a
  child its bootstrap-channel address without any other channel yet. Syscall
  14 was changed in place; there is no legacy `SYS_THREAD_SPAWN2` spelling.
- **init**: loaded by the kernel from the embedded PE blob (pe.c's one
  remaining caller); its death is a kernel panic. hello.c is init and
  the whole ring-3 test suite: it builds children per §5 (create, move,
  share-image-RX — SASOS makes the entry pointer valid cross-AS —
  pre-seeded bootstrap channel, spawn) and exercises channels, local event
  wakes, revoke wakes, kill, tree events, and reap.
- **PID and resource indices** are ordered LLRBs. The global PID registry
  retains zombie structs until exact `PROC_DESTROY`; per-process children,
  threads, owned blocks, and incoming views remain enumerable during
  userspace-driven teardown. Share edges are stable inline values linked from
  both sides. See [enumeration-design.md](enumeration-design.md).

### The umem lock split (2026-07-11)

The single umem lock this doc assumed throughout was split into the
hierarchy described in memory-design.md §5 (`g_umem` control plane →
per-process list locks), with a separate ring-local CQ lock for channel
publication. What it means for the channel machinery
specifically — the authoritative comments live in `umem.h`,
`channel.c`, and `channel_internal.h`:

- **Two planes.** Futex waits and wakes use per-process list locks plus
  address-keyed futex buckets. Ring drains, replays, and teardown stay under
  `g_umem`; the drain's `sq_head` cursor wants a serializer anyway.
- **CQ publication is ring-scoped, uniformly.** The full-check, CQE write,
  `cq_count` bump, and owner-side wake happen under the lock embedded in
  that ring for every scheme. The asymmetry to remember: holding the CQ
  lock does NOT by itself license posting for shares/
  tree — their posts must stay atomic with level-state flips
  (`notified`/`death_notified`) that live under `g_umem`.
- **No false sharing between unrelated rings.** Each CQ lock has exactly
  the ring's lifetime. IRQ-route and IOMMU-fault locks pin that lifetime
  for borrowed-context posts; endpoint destruction detaches those sources
  and crosses the CQ lock before freeing the ring.
- **`channel_block_torn` contract**: callers mutate shared/channel identity
  first (edge pushed/removed, owner swapped, lists unlinked), then wake
  parked threads. Post-mutation parkers fail classification; pre-mutation
  parkers are woken by torn. Scheme creation is the controlled exception: it holds
  the owner's list lock, verifies the private FIFO is empty, and publishes the
  ring before releasing the list lock, so nobody can park in between.
- **Layout**: per-scheme logic moved to `kernel/src/schemes/{shares,
  tree,irq,timer}.c` over the shared internals in `channel_internal.h`;
  `channel.c` keeps the plumbing, data-plane syscalls, drains, and
  teardown.
