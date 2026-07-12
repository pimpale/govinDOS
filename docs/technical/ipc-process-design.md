# IPC & process design: channels, kernel schemes, fault reflection, threads, process trees

Status: **implemented 2026-07-07** — §§1, 2, 4, 5 all landed (channels +
schemes -1/-3, process trees, zombies + reap, kernel-thread deletion,
`g_as_template` collapse); **§3 fault reflection remains unimplemented**
(its own session, as planned). §7 records where the implementation
deliberately diverges. Companion to [memory-design.md](memory-design.md);
builds directly on its ublock/revoke/pristinity model. Where this
document contradicts details of that one (notably the reaper kthread in
its §7 and the eager teardown walks in its §6), this one is what's
implemented — the reaper kthread is gone.

Implementation map: `channel.c/h` (channels, private events, kernel ring ABI,
schemes, waiter slots), `process.c/h` (tree, embryo, kill, zombies,
reap steps), `umem.c/h` (share edges, revoke authority, VM_MOVE
mechanics, live-pid index), `syscall.c/h` (dispatch; SYSCALL/SYSRET),
`userspace/bin/tests/tests.c` (the ring-3 test suite for all of it —
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
  servers defend themselves with their own quota/accounting policy, in
  which time-per-request can participate. The kernel does not arbitrate.
- **Sharing maps immediately; consent is keeping.** `SYS_VM_SHARE` to a
  user pid maps the block into the target on the spot and posts a
  notification (§2, scheme `-1`); rejection is one `SYS_VM_UNSHARE` —
  the same bounded work an accept handshake would cost, with no pending
  state to track. The accepted price is the planting caveat (§1): the
  mapping and its bookkeeping exist until the target unshares it. Resource
  budgets are deferred to the capability design.
- **Teardown has one authority: the revoke path.** Free, unshare,
  decline, and death all land in the same umem code, which wakes parked
  peers with an error. There is no parallel IPC bookkeeping to keep in
  sync with block lifetime.
- **Death is O(1); the dead are zombies until their parent reaps them.**
  Pids are u64, allocated monotonically, never reused. Cross-process
  references are `(pid, base)` pairs checked against a live-pid index,
  not pointers, healed lazily. Reclamation is the parent's reap loop —
  bounded syscalls, destruction mirroring construction — so there is
  **no kernel work queue** and no deferred kernel work at all (§4).
- **Fault reflection is in** (separate implementation session). The
  debugger requirement alone justifies it; surviving revoked channel
  memory is its first user. Longjmp-shaped: no instruction skipping.
- **No kernel threads.** The only per-CPU kernel contexts are the
  scheduler/idle loops on the bootstrap stacks. Everything currently done
  by kthreads is dissolved (§4). Nothing is deferred inside the kernel,
  either: teardown — the one long-running job — is the parent's reap
  loop, so the scheduler/idle loops are pure dispatch.
- **Process trees, parent-driven creation, no reparenting ever.**
  Parents build children from their own resources (embryo state, block
  ownership transfer, explicit first-thread spawn). **Killing a process
  kills its descendants recursively.** Orphaned embryos die with their
  parent by the same rule; a dead parent's zombies are reaped by the
  grandparent as part of its subtree. Daemonization means asking init
  (over a channel) to spawn the image as *its* child — Unix's
  orphan-adoption by pid 1 is deliberately rejected; Fuchsia's job
  trees are the precedent.
- **Resource budgets:** deferred to the capability design. The kernel has no
  uid/account layer today.

## 1. Shared-block channels

A channel is a shared block that both sides agree to treat as a ring.
The kernel contributes a consented way to establish the share and a
park/wake pair scoped to the block. Between user peers, everything
inside the block is userspace convention; when the far end is a kernel
scheme (§2), the same two data-plane syscalls apply and the layout is
kernel ABI.

Kernel-side state lives entirely on existing umem structures: two
waiter slots per ublock (one for the owner, one for the sharer), a
notified bit per share edge, and per-scheme endpoint state
for kernel channels (§2). The ublock sharer machinery is the single
source of truth for who participates, and its revoke path is the single
teardown authority. There is no session object, no per-process session
list, no IPC registry. (Locking, as implemented 2026-07-11: the wait/
doorbell data plane runs on per-process list locks + per-block stripe
locks and never takes the global umem lock — see §7's lock-split entry
and memory-design.md §5.)

### Syscalls

`SYS_VM_ALLOC`, `SYS_VM_FREE`, `SYS_VM_PROTECT`, `SYS_VM_UNSHARE` are
unchanged from memory-design.md. (Renamed from `VM_MAP`/`VM_UNMAP`
2026-07-11: in a SASOS everything is already mapped in principle —
these verbs create and destroy blocks, they don't position anything.)

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

- `SYS_BLOCK_DOORBELL(base) -> 0` — role-inferred, never blocks.
  - Owned block with no sharers or scheme: wakes the owner's waiter slot.
    This is a process-private event block; userspace supplies the durable
    sequence/queue state and may use it to wake another thread locally.
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
    userspace (lib/sys kring) re-rings until the `sq_head` mirror
    catches up to what it published.

- `SYS_BLOCK_WAIT(addr, expected) -> 0` — (may park.) `addr` is any
  4-byte-aligned address inside a block the caller has a view of
  (`SYSERR_INVAL` otherwise). Under the block's stripe lock: load the
  32-bit word at `addr`; if it differs from `expected`, return 0
  immediately; else park in the caller's side slot until a wake: the
  local or peer doorbell or, on a kernel channel, the kernel posting a CQE
  (returns 0); or revocation/identity change (`SYSERR_DEAD`). Parking on a user channel
  whose peer process is no longer live fails `SYSERR_DEAD` up front (a
  live-pid index check, §4) — only threads already parked at the death
  wait for reap-time revocation. Doorbell, post, and park all take that
  same stripe, so a wake between the caller's last look at the word and
  its park cannot be lost — the protocol is verbatim the old
  `ring_wait_user`, generalized to a caller-chosen word. The loaded word is untrusted:
  the kernel only compares it, and a lying peer can only misdirect
  waiters who chose to rendezvous with it. `SYSERR_EXIST` if the side
  already has a waiter — one thread per side, or per private block;
  multi-threaded endpoints shard across blocks.

An owned block with zero sharers is the process-private case. A user↔user
channel has exactly one sharer. Both data-plane calls return `SYSERR_INVAL`
on a block with several sharers or when the caller is neither participant.
A kernel channel's far side is the scheme; it has no sharer entry. The
first share changes private-event identity into channel identity and wakes
any prior private waiter with `SYSERR_DEAD`.

### Establishment flow

```
server:  VM_ALLOC -> ch; VM_SHARE(ch, -1)      // share channel, once
         BLOCK_WAIT on it; EV_SHARE {pid, base|order} arrives (mapped)
         validate size/peer — VM_UNSHARE if unwanted —
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
rejection is `SYS_VM_UNSHARE`, an
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
`(pid, base)` pair is forever unambiguous. A live-pid index (hash map,
or sorted-by-pid vec with binary search — pids insert in order, so
append is O(1) and lookup O(log n)) maps pid → `struct process *`.
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

**Death is O(1).** Mark dying, remove from the live-pid index, post
`EV_CHILD_DEAD` to the parent's tree channel (§2 — one bounded post;
skipped if the parent is itself dying). Threads die at their next
kernel entry or are woken from parks with a dead result, each TCB
freed inline, O(1) apiece. When the last CPU switches away from the
dying AS, `as_switch` flips a drained flag (idle already switches to
`g_as_kernel`, so this terminates). Killing a subtree marks and
park-wakes each victim thread — O(own subtree), the one deliberately
super-constant kill cost.

**Zombies hold everything.** Pristinity needs no revocation at death:
revocation guards *recycled* memory, and a zombie's blocks are still
allocated. Sharers' views survive — a server may drain a dead client's last
requests. Parents are responsible for promptly reaping their dead subtrees;
resource containment is deferred to capability budgets.

**`SYS_PROC_REAP(pid) -> REAP_MORE | REAP_DONE | SYSERR_AGAIN`** —
callable by the parent on a dead child, covering the child's whole
dead subtree (post-order cursor; nothing ever reparents, so the
subtree is closed). One bounded step per call, whichever applies:

1. revoke + free one owned block of the deepest unreaped zombie,
   waking parked sharer-side threads `SYSERR_DEAD` / posting `EV_DEAD`
   to their registrations — exactly the §1 revoke path; or
2. drop one shared-in view, waking the owner-side waiter parked for a
   doorbell that will never come (the zombie's own *views* need no
   unmapping — they die with its AS); or
3. free K page-table nodes of the AS — `SYSERR_AGAIN` until the drain
   flag is up; or
4. all resources gone: free the metadata vec in one
   shot, free the process struct — `REAP_DONE`.

A block can instead be **claimed** with the upward `VM_MOVE` (§5)
before its reap step frees it — post-mortem inspection and exec-image
recycling built from the same primitives, construction run in reverse.
Threads already parked on a zombie's channels at death wait for the
reap-time revocation; anyone parking afterwards fails fast on the §1
liveness check. That latency rides on prompt parents — a libc default
(park on the tree channel, reap on `EV_CHILD_DEAD`), not a kernel
guarantee — accepted.

What stays lazy forever: a dead sharer's entry in a live owner's
sharer vec (dropped when the owner next walks it), share edges naming
the dead (revalidated on touch), and the live-pid index hole itself.

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
SYS_PROC_CREATE()                        -> pid   // embryo: AS clone, no threads
SYS_VM_MOVE(base, pid)                            // ownership transfer along tree edges only (below)
SYS_VM_PROTECT(base, len, prot [, pid])           // parent may set an embryo's views (W^X after writing)
SYS_THREAD_SPAWN(pid, entry, stack_top)  -> tid   // parent+embryo only; first spawn seals the child
SYS_PROC_KILL(pid)                                // own descendant; the subtree dies (O(subtree) mark+wake)
SYS_PROC_REAP(pid)     -> more | done | again     // own dead child; one bounded step (§4)
```

- Pids are the u64 never-reused identifiers of §4; `SYS_PROC_CREATE`
  is where they are minted.
- Parent allocates blocks in its own AS, writes the PE image (loader =
  today's pe.c logic as a userland library; relocs computed against
  final addresses), transfers the image + stack blocks, sets per-section
  W^X on the embryo's views, spawns the first thread. First spawn ends
  embryo state; parent authority drops to normal peer.
- `VM_MOVE` runs only along parent↔child tree edges: **down** into your
  own embryo (construction) and **up** out of your own zombie child
  (reap-time claim — post-mortem inspection and exec-image recycling
  are userspace libraries built on this). General gifting between
  established processes stays banned — a griefing vector and unneeded;
  established processes *share* (channels; an unwanted share costs its
  target one unshare, while a move changes ownership, which is why only
  moves are tree-restricted). A parent can
  pre-seed a bootstrap channel (to itself or a registry server) before
  first spawn — the seL4/Xen answer to how strangers ever get
  introduced.
- `SYS_THREAD_SPAWN(self, ...)` post-embryo gives in-process
  multithreading with no extra mechanism.

### The tree

Every process records its parent and children. **Kill is recursive**:
killing a process kills its descendants, embryos included (which also
answers the orphaned-embryo problem — an embryo has no threads, so
nthreads-driven death can never fire for it; tree death is what reaps
it). Kill delivery: mark dying; parked threads get an error/dead result;
running threads die at next kernel entry (for CPU-bound hostile children
the quantum timer bounds that entry to one quantum — see §7).

**Nothing ever reparents.** Death follows tree edges down; reaping
follows them up; the tree is never restructured. A dead parent's
zombies are reaped by the grandparent as part of the parent's subtree
(§4's post-order cursor makes this the same loop, not a special case).
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
   embryo, `VM_MOVE` both directions, parent protect/spawn, recursive
   kill, zombies + `SYS_PROC_REAP`, the `-3` tree scheme; pe.c →
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

- **Timer preemption is implemented (2026-07-08).** A LAPIC one-shot
  (VECTOR_PREEMPT=0xFB) is armed by the scheduler loop before every
  dispatch (fresh 10 ms quantum per dispatch) and disarmed on the idle
  path, so idle is tickless. Expiry from ring 3 is an involuntary
  SYS_YIELD: the handler EOIs, runs the death checkpoint, saves the trap
  frame into the TCB and parks-requeued — the same path as a voluntary
  yield, so nothing downstream can tell the difference. The kernel is
  never preempted: IA32_FMASK masks IF for the whole syscall path, so a
  mid-syscall expiry stays pending until sysret and lands one
  instruction into ring 3 (syscalls are implicit critical sections;
  worst-case preemption latency = longest syscall). A shot landing in
  the scheduler loop's own IF=1 windows is treated as spurious. The
  timer is calibrated once on the BSP against polled PIT channel 2 (the
  APIC timer clocks off the shared bus clock). Preemption at arbitrary
  ring-3 instructions is what forced eager FPU handling: fxsave64 into a
  512-byte TCB area at every frame save, fxrstor64 at resume. x87/SSE
  only — userspace stays -mgeneral-regs-only / AVX-free until XSAVE
  lands. This also closed the last reap gap: a CPU-bound thread of a
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
- **Reap step 3 frees the whole AS in one call**, not K page-table nodes
  per call — as_free isn't incremental yet. Bounded by the AS's table
  count, which sharer/owner teardown in earlier steps has already pruned.
  Revisit if page trees ever get big enough to matter.
- **Reap also gates on culled TCBs** (`nthreads == 0`, SYSERR_AGAIN
  otherwise): killed threads die at their next kernel entry or at
  dispatch (the scheduler culls dead-process threads before installing
  them), and the process struct must outlive every TCB.
- **All death cascades.** The doc said kill is recursive; the
  implementation treats natural death (last thread exits) identically —
  descendants never outlive their ancestor, embryos included. Unifying
  the two keeps "the dead subtree is closed" unconditional, which the
  post-order reap cursor relies on.
- **VM_MOVE**: the receiver's view arrives R|W (parent applies W^X via
  VM_PROTECT-with-pid afterwards); kernel-channel blocks refuse to move
  (a scheme endpoint is owner-bound identity); sharer edges survive the
  move; a receiver's pre-existing shared-in view is subsumed. Any move
  wakes parked waiters SYSERR_DEAD — ownership is channel identity.
- **Sharing changes wait identity at both boundaries.** The first share
  turns a private event block into a channel; the second makes it ordinary
  multi-sharer memory. Either transition wakes existing waiters SYSERR_DEAD.
- **Resource budgets are not wired**: there are no kernel uid accounts or
  per-process limits. A later capability design may add explicit budgets.
- **Wait-groups were removed (2026-07-12).** Scheme `-2`, its ABI, channel
  registration slots, cross-block forwarding, and two-stripe ranked wake
  path are gone. Process-private ublock events plus sharded server queues are
  the userspace building blocks for multiplexing. Existing scheme ids were
  intentionally not renumbered.
- **SYS_THREAD_SPAWN carries an `arg`** (4th syscall argument) delivered
  in the child thread's first argument register — how init hands a
  child its bootstrap-channel address without any other channel yet.
- **init**: loaded by the kernel from the embedded PE blob (pe.c's one
  remaining caller); its death is a kernel panic. hello.c is init and
  the whole ring-3 test suite: it builds children per §5 (embryo, move,
  share-image-RX — SASOS makes the entry pointer valid cross-AS —
  pre-seeded bootstrap channel, spawn) and exercises channels, local event
  wakes, revoke wakes, kill, tree events, and reap.
- **Live-pid index** is the linear registry vec from umem.c (processes
  are few; binary search when it hurts). Sharer entries and shared_in
  still hold `struct process *` / `ublock *` pointers rather than
  `(pid, base)` pairs — safe today because zombies keep their structs
  until reaped and every edge is unlinked by then; the doc's healed-lazy
  pair scheme becomes necessary only if anything ever caches references
  across the reap boundary.

### The umem lock split (2026-07-11)

The single umem lock this doc assumed throughout was split into the
hierarchy described in memory-design.md §5 (g_umem control plane →
per-process list locks → per-block stripe locks, all
shootdown-servicing). What it means for the channel machinery
specifically — the authoritative comments live in `umem.h`,
`channel.c`, and `channel_internal.h`:

- **Two planes.** `SYS_BLOCK_WAIT` and `SYS_BLOCK_DOORBELL` on user
  channels and private event blocks run on list lock + one stripe only;
  unrelated blocks never contend. Ring drains, replays, and teardown stay
  under `g_umem`; the drain's `sq_head` cursor wants a serializer anyway.
- **CQ publication is stripe-scoped, uniformly.** The full-check, CQE
  write, `cq_count` bump, and owner-side wake happen under
  stripe(ring block) for every scheme. The asymmetry to remember:
  holding the stripe does NOT by itself license posting for shares/
  tree — their posts must stay atomic with level-state flips
  (`notified`/`death_notified`) that live under `g_umem`.
- **No current channel path holds two stripes.** A future cross-block
  operation must pin both blocks and acquire stripe indices in ascending
  order; the old registration-specific release/reacquire/revalidate path
  was removed with scheme `-2`.
- **`channel_block_torn` contract**: callers mutate channel identity
  first (edge pushed/removed, owner swapped, lists unlinked), then
  call torn *without* the stripe — torn takes it itself and wakes parked
  threads. Post-mutation parkers fail classification; pre-mutation parkers
  are woken by torn. Scheme creation is the controlled exception: it holds
  the owner's list lock while tearing a private waiter and publishing the
  ring, so nobody can re-park in between.
- **Layout**: per-scheme logic moved to `kernel/src/schemes/{shares,
  tree,irq}.c` over the shared internals in `channel_internal.h`;
  `channel.c` keeps the plumbing, data-plane syscalls, drains, and
  teardown.
