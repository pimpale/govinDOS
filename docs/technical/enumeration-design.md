# Enumeration design: walkable edges and parent-driven teardown

Status: **implemented 2026-07-28**. Amends
[memory-design.md](memory-design.md) §5–§7: the sharer vec and the
`shared_in` vec are replaced by one dual-linked edge object, and the
owner-death policy of §6 ("revoke, option 1") keeps its outcome but
changes its agent — the kernel no longer frees a dead process's owned
blocks during destruction; the reaper dismantles them with the enumeration
calls below. Amends [iommu-design.md](iommu-design.md): DMA mappings
gain a block-side index and a coerced revoke verb. Narrows
[capability-design.md](capability-design.md): grant enumeration is
explicitly *not* built (§7 below). Extends the reach of
[futex-design.md](futex-design.md) §5: with automatic block destruction gone,
"disorderly teardown is the parent's" is now the only teardown path for
owner death, not the fallback.

Implementation map: `abi/gdosabi/syscall.h` (five new `SYS_VM_*`
entries in the vm ability group plus a pid argument on
`SYS_VM_DROPSHARE`, and `SYS_THREADS`/`SYS_THREAD_DESTROY` in the
proc/thread group), `kernel/vendor/llrb`
(identity-preserving remove — successor splice replaces the payload
copy — plus `_get_ref`/`_iter_next_ref` borrow accessors and a
randomized `_valid` selftest), `kernel/src/umem.{c,h}` (edge object,
`llrb_pid_edge`, `llrb_base_block`, and `llrb_base_edge`
instantiations of the owned vendor template, the `g_ublocks` global
base index, gather wrapper, four vm enumerators, reaper-authority
predicate, pid registry retaining zombies until final destroy,
reap-step deletion),
`kernel/src/schemes/shares.c` (pending-list drain replaces the
`shared_in` walk), `kernel/src/iommu.c` (block-side mapping index —
`llrb_domid_map` instantiation — monotonic domain ids, coerced
revoke), `kernel/src/process.c`
(`umem_reap_one_block_locked`, `umem_reap_one_view_locked`,
`iommu_reap_one_locked`, and the TCB-cull loop deleted from the step
chain along with `irq_reap_one_locked` (driver-side fusion — no
unbound route state remains); `SYS_THREADS` and
`SYS_PROC_CHILDREN` enumerators with the `llrb_tid_thread` and
children-tree conversions; `SYS_THREAD_DESTROY(pid, tid)`; final step
renamed from `SYS_PROC_REAP` to `SYS_PROC_DESTROY` and gated on "owns
nothing, views nothing, has no children, and has no TCBs"),
`kernel/src/schemes/irq.c` (release/destroy free `present` pin
routes; `KIRQ_MSI` fused allocate+bind on the calling ring with
the shared wildcard route token; `KIRQ_BIND` deleted; `KIRQ_MSI_ADDR`
token-authenticated address/data query; `{slot, generation}` recorded
in concrete route grants and verified at every slot-scoped use),
`kernel/src/capability.c` (grant decoupling: `creator`/creator-list
fields and `created_grants` deleted; `cap_reap_one_locked` deleted;
bearer `KCAP_REVOKE`; no quota subsystem),
`packages/gdos-syscalls` (typed syscall wrappers) and
`packages/gdoslib-dev` (recursive `pe_destroy` choreography).

## 0. Decisions log

- **Retention must be enumerable by the party that has to dismantle
  it.** `VM_FREE` and the final destroy step refuse while sharers, DMA
  maps, or pins exist — but a bare count (`dma_pins`) or a private vec
  can only *block* teardown, not *direct* it. Every retention mechanism
  that a third party (the reaper) must dismantle becomes a set of edge
  objects walkable by a u64 key. Counts remain only where nobody
  outside the kernel ever dismantles (`thread_pins`: completion
  registrations die with their TCBs, which `SYS_THREAD_DESTROY`
  disposes before any block teardown — §5's ordering; and a *live*
  owner blocked by `thread_pins` needs no enumeration because it
  created every registration itself at `THREAD_SPAWN` — the same
  self-tracking argument as grants).

- **The cursor is the element key, not a position.** Enumeration calls
  are stateless: "up to `cap` keys strictly greater than `after`,
  ascending." Keys are pids (sharers), block bases (owned blocks and
  incoming views), domain ids (DMA maps), and tids (threads) — all
  u64, all unique per set, and all stale-safe: pids, domain ids, and
  tids are never reused, and block bases (which the buddy *does*
  reuse) are re-bound to the current object by the authority check, so
  a stale base can only fail or name a block the caller legitimately
  holds (§2). Rejected: index cursors (unstable under concurrent removal —
  a skipped sharer is undetectable and unresolvable), kernel-held
  cursor handles (per-caller kernel state with a lifetime, in an OS
  with no descriptor table to hang it off), and node-pointer cursors
  (use-after-free the moment a concurrent detach lands).

- **Separate syscalls, shared plumbing.** One enumerator per kind, not
  a multiplexed `SYS_VM_ENUM(kind, …)`. The multiplexer relocates the
  per-kind dispatch from the syscall table (which exists anyway) into a
  hand-written switch, then adds kind constants, kind validation, and
  wrapper plumbing — while userspace wants distinct typed functions
  regardless. The genuinely shared parts — clamp, buffer validation,
  copy-out, the cursor contract — are one internal wrapper; the
  per-kind parts — permission predicate, lock, root, and a ~8-line
  gather loop over the tree's existing typed iterators — are honestly
  different code and stay different functions, inside and out.

- **Share edges become one pinned slab object, indexed by the existing
  owned LLRB.** Today the relationship is recorded twice with no link
  between the records: `b->sharers` (vec of edge values) and
  `p->shared_in` (vec of block pointers), synchronized by hand at every
  mutation site, with linear finds to cross from one side to the other.
  The edge lives **inline as the value of a node** in the block's
  pid-keyed owned LLRB (the existing vendor template, no new variant),
  carrying the pending links in node storage; the target's view side
  becomes a base-keyed secondary-index tree pointing at the same edges
  (§3). This is
  legal only because of a planned template change: **remove becomes
  identity-preserving** — the two-child case splices the successor
  *node* into the matched node's position instead of copying its
  key/value payload, upgrading the contract from "node identity is not
  part of the contract" (llrb_impl.h) to "node addresses are stable
  from insert to remove." The change is ~15 lines, shape- and
  color-identical to the payload-copy result (the LLRB invariant
  arguments carry over unchanged), strictly strengthens `_extract`
  (you get *the* matched node), and can break no existing caller
  (holding node pointers across removes was previously forbidden). Its
  risk — pointer bookkeeping in the subtlest vendor code — is fenced
  by `_valid` (which checks parent links, order, color rules, and
  count) under a randomized insert/remove/extract selftest. Inline
  struct values also need borrow accessors (`_get_ref`,
  `_iter_next_ref`) beside the copy-out ones. The template's
  `_insert_node`/`_extract` give pre-allocated no-OOM insert and node
  reuse — the same idiom the futex buckets already use.
  Cross-structure desync becomes unrepresentable; every detach path
  loses its scans. Rejected: a new intrusive template variant (the
  strengthened owned tree achieves the same layout with ~15 changed
  lines instead of a second template); pointer-to-edge values (correct,
  but a second slab object and a hop per lookup bought nothing once
  remove preserves identity).

- **KEV_SHARE keeps level-state semantics but gains a pending list.**
  "An un-notified edge IS the queued event" stays — it is what makes
  pending memory bounded by live edges, revocation self-cancelling,
  delivery allocation-free, and payloads always-current. What goes is
  the *scan* for un-notified edges: they sit on an intrusive per-target
  pending list, membership ⇔ un-notified. Rejected: an overflow vec of
  event copies — same drain cost, but reintroduces churn-unbounded
  growth (or a dedup scan), stale delivery after revocation, and an
  allocation at notify time charged to nobody sensible.

- **Invariant: no umem critical section does work proportional to edge
  count.** Drains do O(CQEs posted) bounded by CQ room; enumeration
  does O(log n + batch) bounded by the batch clamp; teardown is
  O(edges) *total* but chunked — intrusive membership is the resume
  state, so any prefix of progress is durable and the lock can drop
  between chunks. This is the same amortization discipline as
  `RING_SQ_BATCH`/`RING_SQ_CHUNK`.

- **Owned blocks outlive their owner; the reaper dismantles them.**
  The reap step that freed a zombie's owned blocks is deleted. The
  final destroy step fails `SYSERR_EXIST` while the zombie still owns
  blocks — the same single-transaction shape as `VM_FREE` refusing
  while sharers exist. Rejected: transferring ownership to the reaper
  at death (provenance loss — the reaper can no longer tell inherited
  blocks from its own, and the zombie's memory becomes part of the
  reaper's owned set at a moment it didn't choose; also leaves nothing for
  enumerate-by-pid to walk).

- **Views and threads are enumerable too, and destroy keeps only what
  userspace cannot reach.** A process can enumerate its own incoming
  views (`SYS_VM_VIEWS`) and threads (`SYS_THREADS`) — delegation to
  userspace wants the full picture of what a process holds — and its
  reaper can enumerate a zombie's and dismantle each:
  `VM_DROPSHARE(base, pid)` coerces a view away,
  `SYS_THREAD_DESTROY(pid, tid)` disposes a TCB (running the futex claim
  protocol kernel-side). That deletes the view-revocation and TCB-cull
  reap steps; the iommu endpoint step goes too (domains die in the
  revoke path when the reaper `VM_FREE`s the zombie's ring blocks,
  which the choreography does anyway). The irq step is deleted outright by
  **driver-side fusion** (irq-design, pci-design §7): `KIRQ_MSI`
  becomes a fused allocate+bind executed by the driver on its own IRQ
  ring, so a route slot is either free or bound to a ring — the
  allocated-unbound state no longer exists, for MSI exactly as for pin
  claims (which were always fused). All route cleanup is ring cleanup:
  release/destroy free slots (including `present`; a level pin is
  already masked, its ack never comes, and the next claimant
  reprograms the RTE; MSI quiescence follows `pcid`'s mask/reset order
  and retains the accepted pre-IR stale-source caveat below), and the
  reaper reaches every route via `VM_FREE` of ring blocks. Rebinding
  is release + re-fuse; there is no sweep because there is nothing
  outside a ring to sweep. Stale authority across slot recycling is
  generation-gated (the payload invariant's shape (b), below) —
  grants are API keys, never lifecycle records, so the grant tree
  plays no part in slot cleanup. In v1 `pcid` copies the same wildcard
  IRQ token to every trusted driver; `KIRQ_MSI` is the one operation
  that token authorizes for MSI, and a driver can exhaust the fixed
  vector pool. There is deliberately no quota or accounting layer.
  Interrupt remapping later expands the source-indexed slot namespace,
  reducing this risk while closing the spoofing gap. The residue is
  then just **AS free and struct release** — with grants decoupled from
  processes (below), no abstract state of any kind is disposed at
  death; the process lifecycle ends by tearing down only the process's
  own body.

- **Grants are decoupled from processes; destruction touches no grants.**
  The creator tie is deleted (capability-design): grant nodes carry no
  process pointer; `created_grants` and the reap drain
  are gone. This is safe *now* — it was rejected twice
  earlier, and the objections dissolved one by one as the design
  moved: no resource cleanup rides the drain (driver-side fusion made
  slot cleanup ring-owned; grants are API keys), no block waits on
  grants (retention deleted), and every payload shape fails safe in a
  dead holder's hands
  (eternal identities or generation-gated). A dead holder's anchors
  persist as inert garbage until collected — by any
  copy-holder (**revocation authority is token possession**, the
  bearer-revoke expansion accepted deliberately: copy-holders already
  share one fate, and token theft already means full use-authority)
  or, once the deferred GC surface lands (subgrant enumeration on the
  `-2` ring, minting public grant ids), by ancestor-token holders,
  init at the root. Nothing gates on GC: teardown never waits for
  grants, so collection is hygiene. Live holders self-track what they
  minted; bearer tokens make holder enumeration meaningless in
  principle. Capability-design §9's ownership *transfer* loses its
  motivation — there is no creator tie left to transfer.

- **Grants retain no blocks.** Capability-design's planned rule that
  `VM_FREE` refuses a block still anchoring grants is deleted (it was
  never implemented — the real check is pins/sharers/destroyable
  only). The hazard it guarded against cannot occur with the grant
  payloads that exist: devmem grants hold `{base, len}` device ranges
  *by value* — eternal physical identities never recycled into
  different meanings — and irq/iommu grants reference a static route
  slot and a requester id. Nothing dangles when a block dies, and
  re-redeeming a surviving grant maps the same registers the authority
  always named; prospective authority and established objects are
  separate planes in both directions. What the rule actually guarded
  splits into its proper homes: alias prevention becomes a
  **redemption-time check** (`VM_MAP_DEVICE` refuses a range
  overlapping a live device block, via `g_ublocks`), and route slots
  are ring-owned under driver-side fusion, so no grant/ring teardown
  ordering exists at all. Invariant going forward, one of three shapes per grant type:
  **(a) the payload is a never-recycled identity** (device ranges,
  requester ids); **(b) the payload is a generation-gated recyclable
  identity** (MSI route slots: the concrete grant records
  `{slot, generation}` and every use — the `KIRQ_MSI_ADDR`
  derivation foremost — verifies it, so authority minted for a slot's previous incarnation
  fails harmlessly against its next); or **(c) the payload references
  a recyclable object through an ordinary enumerable retention.**
  Nothing else. The companion principle, stated once here: **grants
  are API keys — they entitle the holder to create kernel objects,
  and are never an object's lifecycle record.** Revocation is
  prospective, uniformly; the created objects (slot reservations,
  bindings, blocks, mappings) are owned by processes and rings and
  torn down by the ordinary teardown machinery. (Contrast seL4, where
  capabilities are kernel objects and deletion rides the derivation
  tree — our tokens are bearer data with thin revocation anchors, and
  loading object ownership onto the anchors would drag them back
  toward kernel-object capabilities.) This also makes
  capability-design's `KCAP_LIST` TODO moot: nobody ever needs to ask
  "which grants does this block anchor" because the answer never
  gates anything.

## 1. The problem

Three mechanisms keep a block alive past a `VM_FREE` or a reap attempt:
sharer edges, DMA mappings, and transient pins. The futex transition
made teardown a userspace choreography (sentinel → wake → drop → free),
and the pci/iommu design made `VM_FREE` refuse while DMA-pinned. Both
refusals are correct; neither is *actionable*, because the party that
must act cannot ask the kernel what is in the way:

- The owner (or its reaper) cannot list a block's sharers to issue
  per-edge `VM_UNSHARE(base, pid)`.
- Nobody can list which IOMMU domains map a block; the block's entire
  record is the `dma_pins` counter. A DMA-pinned block owned by a
  zombie deadlocks teardown outright.
- A reaper cannot list a zombie's owned blocks — or its incoming
  views — at all, and a live process cannot even list its own.

Meanwhile the kernel still frees a dead process's owned blocks inside
`PROC_REAP` steps — automatic work of exactly the kind the futex design
removed elsewhere, and the reason the missing enumeration has not hurt
yet. This design removes the automatic path, renames the remaining
final-body operation `PROC_DESTROY`, and supplies the APIs the
replacement choreography needs.

## 2. The enumeration ABI

Six enumerators plus the coercion/disposal verbs — the vm ones in the
vm ability group, the thread/process ones in the proc/thread group:

```
SYS_VM_SHARERS   (base, buf, cap, after) -> count   owner or reaper
SYS_VM_BLOCKS    (pid,  buf, cap, after) -> count   self or reaper
SYS_VM_VIEWS     (pid,  buf, cap, after) -> count   self or reaper
SYS_VM_DMA_MAPS  (base, buf, cap, after) -> count   owner or reaper
SYS_THREADS      (pid,  buf, cap, after) -> count   self or reaper
SYS_PROC_CHILDREN(pid,  buf, cap, after) -> count   self or reaper
SYS_VM_DMA_REVOKE(base, domain_id)       -> 0       owner or reaper
SYS_VM_DROPSHARE (base, pid)             -> 0       self (pid 0) or reaper
SYS_THREAD_DESTROY(pid, tid)             -> 0       same process (pid 0) or reaper
SYS_PROC_DESTROY (pid)                    -> PROC_DESTROY_MORE/DONE,
                                               SYSERR_AGAIN, or SYSERR_EXIST
```

Two resolution structures make these calls implementable without
subtree scans:

- **A global base index, `g_ublocks`** — a base-keyed secondary index
  (pointer values) over every live ublock, maintained at
  alloc/free/`VM_MOVE` under g_umem. Block-scoped calls (`SHARERS`,
  `DMA_MAPS`, `DMA_REVOKE`, `UNSHARE`, `DROPSHARE`, `FREE`) resolve
  `base → block → owner` through it in O(log n); without it, a reaper
  naming a zombie's base would force a scan of the dead subtree's
  per-process trees. Ublock node storage is stable across `VM_MOVE`
  (the same node moves between owner trees), so the index entry never
  needs rewriting on ownership transfer. The index also serves
  `VM_MAP_DEVICE`'s redemption-time overlap check (§0: alias
  prevention moved from free time to redemption time).
- **The pid registry retains dead processes, but raw and live lookup
  are distinct APIs.** The global pid tree indexes every allocated
  process struct until `SYS_PROC_DESTROY` removes it. A raw
  `proc_lookup_any_locked(pid)` is used only by enumeration and
  destruction; the ordinary `proc_lookup_live_locked(pid)` wraps it
  and rejects `process_is_dead(p)` and an already-freed AS. Every
  registry-wide walker must choose one contract explicitly: live
  walkers skip dead/AS-freed entries, while teardown walkers tolerate
  them. This prevents keeping zombie pids resolvable from silently
  widening old “pid lookup implies live AS” call sites. The registry
  lock pins the struct; removal occurs only in the final destroy step.

`SYS_PROC_CHILDREN` returns the child pids of `pid` (the `children`
vec becomes a pid-keyed tree like the other enumerable sets). It
exists because death notification is deliberately shallow — only
directly dead children post `KEV_CHILD_DEAD` to a live parent
(tree.c); interior zombies of an announced subtree never notify. The
reaper therefore discovers the dead subtree top-down, then performs
cleanup and `PROC_DESTROY` **post-order**. `PROC_DESTROY` refuses a
process whose child tree is non-empty.

`SYS_THREAD_DESTROY` takes `(pid, tid)` because tid lookup is through
the per-process thread tree — there is no global tid registry, and
adding one for this call alone is not warranted.

`SYS_VM_VIEWS` returns the bases of blocks shared *into* `pid`;
`SYS_VM_DROPSHARE` grows a pid argument (0 = the caller, as today) so
a reaper can drop a zombie's view. Either party can still clear a
share edge — the owner via `VM_UNSHARE(base, pid)`, the viewer (or its
reaper) via `VM_DROPSHARE` — and the two verbs converge on the same
edge teardown.

`SYS_THREADS` returns the tids of `pid`'s undisposed TCBs.
`SYS_THREAD_DESTROY(pid, tid)` is **disposal only in v1**: the target
must already be non-running — its process dead, or the thread itself
exited. There is deliberately no "kill a live peer thread" here: the
existing death checkpoints are process-wide (`process_is_dead` at
kernel entry; TCBs carry no per-thread kill-request flag), so
single-thread kill needs a new per-TCB flag checked at the same
checkpoints plus scheduler arbitration — that belongs to the deferred
debug surface, and the choreography never needs it (a zombie's
threads are all dead already). Against a dead target the call is
retry-shaped: a TCB the scheduler still holds (on-CPU winding down,
or `THREAD_DEAD` pending the scheduler's handoff — the scheduler is
never coerced, ownership arbitration is the existing
`wake_state`/handoff protocol) returns `SYSERR_AGAIN`; once the TCB
is off-CPU and blocked, the call wins the futex claim
(`PARKED → CLAIMED`, node removal), disposes its completion
registration **without publishing into dead userspace**, and frees the
TCB — one bounded disposal. Completion ownership is centralized in one
exactly-once helper: normal scheduler exit publishes the completion
word and then drops `completion_block->thread_pins`; scheduler cull and
`SYS_THREAD_DESTROY` skip the write but still drop the pin and clear the
TCB pointer. Whichever path wins the existing claim/handoff arbitration
owns that helper call. A scheduler-owned TCB can therefore never strand
a completion pin merely because it was culled instead of joined.
**Tids are the thread handle, by design**: they are minted
from a global monotonic counter (thread.c) — never reused, so a stale
tid can only miss — and authority over them is predicate-based like
every other name in the system, not possession-based. The rest of the
debug surface (context read/write, suspend/resume, and a debug event
ring, which by the schemes rule belongs with the pending
fault-reflection work in ipc-process-design §3) is deferred, but it
will speak these same names under the same predicate model.

`buf` is a u64 array of `cap` entries in the caller's memory
(view-checked like any syscall buffer); the kernel clamps `cap` to
`VM_ENUM_BATCH` (128 — the gather buffer lives on the single per-CPU
kernel stack, 1 KB); `cap == 0` is `SYSERR_INVAL`. Entries are bare
u64 keys: sharer pids, block bases, domain ids, tids, child pids.
`after` is 0 on the first call, the last key received thereafter —
**key 0 is therefore reserved in every key domain**: pids, tids, and
domain ids mint from 1, and base 0 is never a block (physical page 0
is not allocatable). Any future key type must reserve 0 the same way.

The contract, identical for all six:

> Each call returns up to `cap` keys strictly greater than `after`, in
> ascending order. A count less than `cap` (including 0) means the
> enumeration is complete as of that moment. An element present for the
> entire duration of a scan is returned exactly once; elements added or
> removed concurrently may or may not be returned. A stale key — or a
> stale detach verb aimed at one — can never touch an object belonging
> to a **different authority** than the one the caller held it for.

That last guarantee is deliberately narrower than "keys are never
reused." Pids, tids, and domain ids never recur, but an *attachment*
identity can: a live owner may unshare and re-share the same pid, a
domain may unmap and remap the same block, a freed base may be
reallocated. A stale detach can therefore hit a **recreated attachment
— but only one recreated by the same authority that issued the stale
verb** (only the owner re-shares its block; only the caller's own
domain remaps; a reallocated base re-binds through the ownership
check). Such races are self-inflicted and self-resolvable. The reaper
flow is immune outright: its subject is dead, `VM_SHARE`/`VM_MOVE`
refuse dead targets, and §5's ordering freezes each set before it is
walked — nothing the reaper detaches can be recreated. Attachment
generation counters were considered and rejected as ABI weight solving
only the self-inflicted case.

Every foreign/resource enumeration holds `g_umem`, which pins every ublock
and serializes share and DMA-map topology. The subject's `ulock` additionally
guards its block and view trees; thread and child membership is lifecycle
state already guarded by `g_umem`. Each bounded gather writes into the stack
buffer. The output range must fit wholly inside one writable caller-visible
ublock (the maximum is 1 KiB). The wrapper holds the caller's `ulock` across
validation and the copy: `g_umem` prevents free/recycling, while `ulock`
prevents view removal or a write-restricting `VM_PROTECT`. Copy-out therefore
cannot scribble on recycled or newly read-only memory and needs no block
stripe. Per call the kernel does O(log n + cap) work; block-scoped calls use
the already-held `g_umem` for the O(log n) `g_ublocks` resolution.

`SYS_VM_DMA_REVOKE` is the DMA analog of `VM_UNSHARE(base, pid)`: it
removes one domain's mapping of the block, IOMMU-unmaps and flushes,
and drops that edge. The affected device's subsequent DMA faults are
reported through the existing fault path to the driver's ring; the
design's posture is already "revoke first, the device faults, the
driver observes" and this adds no new hazard class.

### The reaper-authority predicate

One function, used by the six enumerators, `VM_UNSHARE`,
`VM_DROPSHARE`, `VM_FREE`, `VM_DMA_REVOKE`, `SYS_THREAD_DESTROY`, and
`SYS_PROC_DESTROY`.
Each verb names a **subject process** — the party whose stake the call
touches, which is *not* always a block owner: the block's owner for
`SHARERS`/`BLOCKS`/`DMA_MAPS`/`UNSHARE`/`FREE`/`DMA_REVOKE`, the
*viewer* for `VIEWS`/`DROPSHARE` (the block's owner may well be
alive), and the tid's process for `THREADS`/`THREAD_DESTROY`:

> The caller is the subject, **or** the subject is an effective-dead
> strict descendant and every process on the path from the subject
> through the caller's direct child is effective-dead/non-running. The
> caller need not be the immediate parent, but a living intermediate
> child is a hard boundary: a grandparent cannot act on a dead
> grandchild through it. The immutable process tree makes the path
> unambiguous.

The self arm applies only to the resource verbs. `SYS_PROC_DESTROY`
always requires a strict descendant; a process cannot destroy its own
currently executing body.

## 3. Share edges: one object, both sides

```c
struct share_edge {                 // node value; address stable insert→remove
  struct share_edge *pnext, *pprev; // pending list: to->unnotified, iff !notified
  struct process *to;
  ublock *block;
  bool notified;                    // mirrors pending-list membership
};
// block side: llrb_pid_edge,  key = to->pid,     LLRB_VALUE = struct share_edge
//   (inline — legal once remove is identity-preserving; decisions log)
// view side:  llrb_base_edge, key = block->base, LLRB_VALUE = share_edge *
//   in to->views (secondary index — the block-side tree owns the edge)
```

- `b->sharers` becomes the `llrb_pid_edge` tree (`vec_share_edge`
  deleted); the edge is the node's inline value.
- `p->shared_in` (vec of block pointers) becomes `p->views`, the
  base-keyed `llrb_base_edge` secondary index over the same edges —
  pointer values by §8's rule, since the block-side tree owns the edge
  and an object can be inline in only one tree. This is what
  `SYS_VM_VIEWS` gathers from, and `VM_SHARE` pre-allocates its index
  node alongside the block-side node (still no mid-lock OOM path).
- `p->blocks` (vec of block pointers) becomes `llrb_base_block`, an
  owned tree keyed by base — `SYS_VM_BLOCKS`'s gather needs an ordered
  walk, which the vec cannot iterate by cursor. The ublock is **inline**
  here too (the separate ublock slab goes away): `VM_MOVE` is
  `_extract` + `_insert_node` of the same node — ownership of the
  allocation transfers, the address doesn't change, so every external
  `ublock *` (edges, rings, mappings) stays valid — and the free path's
  unlocked flush phase holds the extracted node in `umem_release` and
  slab-frees it at the end. Inlining makes "every block is in exactly
  one owner's tree" a property of the representation rather than an
  invariant the code maintains: a block cannot exist outside a tree
  except while someone holds its extracted node.
- `p->threads` (vec of TCB pointers) becomes `llrb_tid_thread`, keyed
  by tid — `SYS_THREADS`'s gather needs the ordered walk. Pointer
  values, per §8's rule: this is a secondary index — TCB lifecycle is
  owned by the scheduler's exit/handoff machinery, and the TCB slab's
  alignment requirements (fxsave) argue against embedding regardless.
- Share edges are control-plane topology guarded by `g_umem`; process-side
  trees additionally stay under `ulock` per the existing hierarchy.

What this deletes: `find_edge_to` (shares.c) and its cross-structure
assert; the double bookkeeping at every share/dropshare/unshare/death
site (`remove_block`, the find-and-swap-pops); the "already shared to
this pid" scan in `VM_SHARE` (the insert's descent is the check). What
it costs: two slab nodes per edge — the block-side node carrying the
edge inline (~100 B) and the view-side index node (~48 B), against
16 B packed in the vec — both pre-allocated by `VM_SHARE` before it
takes locks and inserted via `_insert_node`, so there is no mid-lock
OOM path.

The view check (`umem_view_locked`) stops being a linear
`find_containing` scan on either side: both `p->blocks` and `p->views`
are base-keyed trees, so containment is a floor-lookup (greatest base
≤ addr, then a range check) — O(log n) on the futex hot path, where
today it is O(blocks + views). The enumeration requirement forces the
order that the earlier "only if it profiles hot" contingency was
waiting for, so `find_containing` and both vecs are simply deleted.

Pinned means *stable from insert to unlink*, nothing more. No path may
carry an edge pointer across a lock drop; `as_flush_multi`'s
one-round-revoke still snapshots what it needs under the lock.

## 4. KEV_SHARE: the pending-list drain

`shares_replay` today walks every `shared_in` block per doorbell and
linearly searches each block's sharer vec for the caller's own edge —
O(all my edges) even when nothing is pending. It becomes:

```c
// on doorbell / channel creation, under g_umem:
while (cq_has_room(ring) && (e = p->unnotified_head) != nullptr) {
  channel_post(ring, KEV_SHARE, e->block->owner->pid,
               e->block->base | e->block->order, 0);
  pending_unlink(e);          // notified = true
}
```

O(posted), O(1) when idle, no scan. Unlink strictly after a successful
post, so the drain is resumable at any point and can chunk under the
lock exactly like ring SQ drains. Edge creation appends to the pending
list (or posts immediately when there is room — `channel_edge_notify`
keeps its fast path); edge destruction unlinks from wherever the edge
is. "This drop IS the ack" (orderly teardown) falls out of the same
unlink.

The tree channel's `death_notified` pattern is the same idiom with the
zombie children as the pinned entries; it already has no scan problem
(children lists are walked for other reasons) and is not changed here.

## 5. Parent-driven teardown

### What changes in process destruction

All five steps leave the reap chain:

- `umem_reap_one_block_locked` — owned blocks are the reaper's
  choreography (below).
- `umem_reap_one_view_locked` — the zombie's incoming views are
  dropped by the reaper via `VM_VIEWS` + coerced
  `VM_DROPSHARE(base, pid)`. (The owner of each viewed block can
  independently `VM_UNSHARE(base, zombie)`; either party clears the
  edge, so a wedged reaper does not block a live owner's `VM_FREE`.)
- the TCB cull — the reaper enumerates `SYS_THREADS` and disposes
  each with `SYS_THREAD_DESTROY`; the per-TCB kernel work (claim
  protocol, node removal, free) is unchanged, only the driver moves.
- `iommu_reap_one_locked` — everything it does hangs off the
  process's iommu ring, so the scheme `destroy` op (running full
  domain teardown: force-detach, unmap, destroy — bounded by the
  fixed `IOMMU_MAX_*` caps) covers it when the reaper `VM_FREE`s the
  ring block. Note the destroy op must tear down whole domains, not
  just mappings of the zombie's own blocks: a domain may map *other*
  processes' blocks, which the per-owned-block `VM_DMA_MAPS` loop
  never visits.
- `irq_reap_one_locked` — deleted outright by **driver-side fusion**.
  Every route flow now allocates and binds in one op: pin claims
  always did (`claim`), and `KIRQ_MSI` becomes the same shape,
  executed by the *driver* on its own IRQ ring under route authority
  from its start record (pci-design §7). A slot is either free or
  bound — there is no allocated-unbound state, so there is nothing a
  reap step or sweep could ever find. All cleanup is ring cleanup:
  `release` and `irq_endpoint_destroy` free slots outright, with the
  `present` exemption removed. A level pin is already masked; its
  pending ack never arrives, the next claimant reprograms the RTE, and
  stale assertions land in `irq_deliver`'s existing unclaimed-vector
  masking. MSI follows `pcid`'s mask/reset ordering and the accepted
  pre-IR caveat below. Rebinding is release + re-fuse, never an
  unbound window. The address/data pair reaches `pcid` by
  token-authenticated derivation (`KIRQ_MSI_ADDR`,
  generation-checked), never as relayed raw values. Slot recycling
  under live grants is safe because concrete route grants record
  `{slot, generation}` and every slot-scoped use verifies it:
  authority minted for a previous incarnation fails harmlessly —
  grants are API keys (§0's invariant), so the grant tree plays no
  part in slot cleanup. Admission to the shared pool no longer rides
  `pcid`'s syscall monopoly. In v1 every trusted driver receives a copy
  of the shared wildcard token and may allocate until the fixed pool is
  exhausted; there is no
  per-driver quota. On slot reuse: a pin route is quiescent by
  construction (the RTE is masked and any in-flight delivery was
  EOI'd), but a stale device
  may still be programmed with a freed MSI route's address/data and
  fire into the slot's next claimant. Deliberately accepted **because
  it is not a new gap**: without interrupt remapping, any DMA-capable
  device can already forge any MSI (the accepted 0xFEE-spoofing gap
  of the IOMMU design), so a quarantine here would fence one instance
  of a hole that remains open everywhere else. When IR lands and
  closes the spoofing gap, MSI slot recycling gets a real fence
  (quarantine-until-IR-invalidate) as part of that work. IR also makes
  the route namespace source-indexed and supplies many more slots,
  reducing the exhaustion risk of the trusted wildcard-token v1.
  Release and ring destruction clear the binding and increment the
  route generation before publishing the slot free, so even the
  unrecycled free interval rejects the old concrete token.

What remains is only the process's own body: AS free and struct
release. Grants are decoupled from processes entirely (§7) — destroy
touches no abstract state.
By the time the final gate passes, the block, view, and thread sets
are all empty, so the AS free needs no per-view walk — the page trees
are torn down wholesale. The final step — releasing the process
struct and returning `PROC_DESTROY_DONE` — requires that the exact
target own no blocks, hold no views, have no TCBs, and have no
children; otherwise `SYS_PROC_DESTROY` returns `SYSERR_EXIST`.
Page-table teardown remains bounded and may return
`PROC_DESTROY_MORE`; a transient CPU/AS pin returns `SYSERR_AGAIN`.
Scheduler ownership is handled earlier by `SYS_THREAD_DESTROY`, which
returns `SYSERR_AGAIN` without removing the TCB.

`SYSERR_AGAIN` and `SYSERR_EXIST` now mean different things at the
destroy callsite: AGAIN is "a transient CPU/AS pin has not drained" —
just call again; EXIST is "you must dismantle blocks, views, or threads before
this can finish, or destroy children first."

### The choreography

`KEV_CHILD_DEAD` announces only the subtree root; the reaper discovers
interior zombies itself by walking `PROC_CHILDREN` from the announced
child (§2). It snapshots/discovers top-down but cleans and destroys
post-order, so a process's child tree is empty at its final step. For
each zombie pid in that order:

```
1. THREADS(zombie, …)                 cursor loop; THREAD_DESTROY(zombie, tid) each
2. VM_VIEWS(zombie, …)                cursor loop; VM_DROPSHARE(base, zombie) each
3. VM_BLOCKS(zombie, …)               cursor loop over owned bases
4. per base:
     VM_SHARERS(base, …)              cursor loop; VM_UNSHARE(base, pid) each
     VM_DMA_MAPS(base, …)             cursor loop; VM_DMA_REVOKE(base, id) each
     VM_FREE(base)                    now unblocked; ring blocks destroy
                                      their endpoints here (irq claims
                                      released, iommu domains torn down)
5. repeat 3 until VM_BLOCKS returns 0
6. PROC_DESTROY(pid) to completion    AS free and exact struct release
```

`PROC_DESTROY` is driven only at the end for each exact pid and touches
nothing but the process's own body: grants are decoupled from processes (§7), so
there is no drain, no ordering interaction, and no abstract state in
destroy at all. It accepts any strict descendant only when every node
between caller and target is effective-dead; a living intermediate
child blocks the call even if the named grandchild is directly dead.
It never walks or implicitly destroys a subtree.

Termination is unconditional — but it rests on an invariant the
implementation must enforce: **`VM_SHARE` and `VM_MOVE` refuse dead
targets.** Nothing can be pushed *into* a zombie, so with the owner
dead no new threads, blocks, views, shares, or DMA maps of its blocks
can be created — every set is shrink-only, and each loop provably
empties. `VM_UNSHARE` against a pid that dropped itself concurrently,
or `VM_FREE` racing a last `DROPSHARE`, are benign retries inside the
loop.

Two orderings above are load-bearing, not stylistic. Step 1 precedes
block teardown because completion registrations hold `thread_pins`
until their TCBs are disposed — normal exits publish before dropping
the pin, while dead-process destruction drops it without publishing.
Freeing blocks first would deadlock against the threads step. Within
step 4, sharers are unshared before DMA maps are revoked because
creating a mapping requires a view of the block: the last `UNSHARE`
freezes the block's DMA set, so the `VM_DMA_MAPS` loop that follows
sees a set that can no longer grow.

### Consequences, stated

- **Waiters stay parked.** Coerced `VM_UNSHARE` never wakes futex
  waiters (futex-design: no revoke wake). Peers parked on words inside
  a dead owner's blocks remain parked until their own deadlines,
  parents, or protocols act. Futex-design §5 already assigned
  disorderly teardown to the waiters' side; this design promotes that
  from fallback to the only path for owner death, which raises the
  priority of the unbuilt robust-list step (futex-design §10 step 7) —
  cross-process lock protocols must now always plan for "peer died
  holding it."
- **Peers keep usable views of a dead owner's blocks** until the
  reaper coerces. This is new: today reap revokes them automatically.
  It is coherent (the block is alive until freed) and even useful (a
  peer can drain a dead producer's buffer), but a peer that wants
  prompt teardown should `DROPSHARE` when it observes the death on its
  tree channel.
- **A zombie pins its memory until the reaper
  finishes.** A slow or wedged reaper holds the memory; the backstop is
  the tree — killing the reaper hands the (larger) dead subtree to
  *its* reaper, so the blocks are always dismantlable by someone, init
  in the last resort. init's reap loop must therefore speak the full
  choreography above, not just `PROC_DESTROY`.

## 6. DMA mappings become edges

`struct iommu_mapping` (today: domain-side singly-linked list only;
the block knows a bare count) gains the block side:

- A monotonic u64 **domain id**, minted at domain creation, becomes
  the mapping key and the public name for a domain in `VM_DMA_MAPS`
  results and `VM_DMA_REVOKE`. The user-supplied `cookie` remains a
  correlation value in events and is *not* a key: it is chosen by the
  creator, so it is neither unique nor reuse-safe.
- The ublock gains an owned `llrb_domid_map` tree keyed by domain id,
  guarded like `sharers` by `g_umem` (so `VM_DMA_MAPS` and `VM_FREE`
  read it under the lock they already hold). Like share edges, the mapping
  can live inline as the node
  value once remove is identity-preserving — the domain-side list
  links move into node storage, upgraded from singly- to
  **doubly-linked, plus a domain back-pointer**: `VM_DMA_REVOKE`
  reaches the mapping from the block side and must unlink the domain
  side in O(1), not by scanning the domain's mapping list. One mapping
  per (domain, block) pair (already the case), so keys are unique per
  set.
- `dma_pins` is deleted; "DMA-retained" is the block-side set being
  non-empty, and the `VM_FREE`/reap checks read that.

`VM_DMA_REVOKE(base, domain_id)` unmaps, flushes, unlinks both sides,
and frees the mapping — the same operation the domain-side unmap op
performs, entered from the block side with the reaper predicate. Both
verbs converge on one internal teardown function.

With the iommu reap step deleted (§5), a dead driver's domains are
destroyed when the reaper `VM_FREE`s the domain's ring block: the
scheme `destroy` op force-detaches any still-attached devices, which
then lose translation entirely — blocked DMA, faults reported nowhere.
That is the fail-closed posture the IOMMU design already commits to,
and strictly safer than the alternatives (a device of a dead driver
has no one to answer for it).

Sharer edges and DMA edges deliberately remain **distinct types
sharing idioms**, not a unified "retainer" abstraction: their far ends
(process vs domain), teardown verbs, and recovery stories (futex
protocol vs fault delivery) differ, and a merged type would carry a
discriminator everywhere. The sharing is §2's contract, the predicate,
the key discipline, and the gather core.

## 7. Grants: decoupled from process death

See the decisions log. Concretely (capability-design carries the
detail): the creator tie is deleted — `struct grant` loses its
`creator` pointer and creator-list links; `created_grants` leaves the
process struct; the reap drain is gone, and **process destruction
touches no grants at all**. There is no accounting or quota field.
Grant teardown happens
only by revocation, whose authority is **token possession** (any
copy-holder; accepted — copy-holders already share one fate).
Teardown touches nothing outside the grant tree — no free hooks, no
object cleanup — because grants are API keys, never lifecycle records
(§0), and grants never gate block teardown (§0: grants retain no
blocks). A dead holder's anchors persist as inert nodes until
collected; the GC surface (subgrant enumeration on the
`-2` ring, with public grant ids) is intended later and gates
nothing. V1 accepts that trusted issuers can exhaust grant memory;
there is no quota safety argument. No enumeration of grants exists in
*this* design.

## 8. Shared-code inventory

| piece | used by | LoC shape |
|---|---|---|
| identity-preserving LLRB remove + borrow accessors | edges, DMA maps; later `grant` inline (below) | ~15 changed + ~20 new in vendor, + selftest |
| owned-LLRB instantiations (existing template) | `llrb_pid_edge`, `llrb_base_block`, `llrb_base_edge`, `llrb_domid_map`, `llrb_tid_thread`, children tree, `g_ublocks` | ~10 each: declaration + impl include |
| per-kind gather loop over `_iter_lower_bound`/`_iter_next` | one per enumerator | ~8 each |
| clamp/validate/copy-out wrapper (`g_umem` + caller `ulock`) | all six enumerators | ~20 |
| reaper-authority predicate | enumerators, UNSHARE, DROPSHARE, FREE, DMA_REVOKE, THREAD_DESTROY, PROC_DESTROY | ~15 |
| per-kind syscall bodies | one each | ~10–15 each: predicate + lock + root + gather |
| pending-list drain idiom | shares now; irq/iommu fault replay opportunistically | per-scheme, ~10 each |

The irq/iommu fault replays keep their current bool-scan for now (their
device lists are small and hard-bounded); adopting the pending idiom
there is uniformity work, not a prerequisite.

Existing trees and the identity-preserving change: `struct grant`
(capability.c) is the one existing beneficiary — it has the exact
edge shape (slab object, seven external `grant *` links across the
revocation tree and creator list, plus a separate `g_grants` node
pointing at it) and can inline into its id-tree node, dropping one
allocation per grant and one hop from the token-validation path; that
lands with capability work, not here. The other three instantiations
are unaffected for a structural reason: **inline when the tree is the
owning collection** (every live object sits in exactly one tree,
transitions are `_extract`/`_insert_node` node moves, and
creation/destruction bracket membership — edges, DMA maps, ublocks,
grants), **pointers when the tree is a secondary index** onto an
object owned elsewhere (`pid_process` indexes processes owned by the
process tree; `futex` and `tdeadline` hold ephemeral registrations of
TCBs owned by the scheduler structures; the new `llrb_tid_thread` and
`llrb_base_edge` are secondary indexes by the same rule).

## 9. Implementation order

1. Vendor LLRB: identity-preserving remove (successor splice),
   `_get_ref`/`_iter_next_ref`, randomized `_valid` selftest. Pure
   contract strengthening; no caller changes.
2. Share-edge conversion: `llrb_pid_edge` (inline edge values),
   `llrb_base_edge` (view-side secondary index), `llrb_base_block`
   (inline ublock values; the ublock slab and `vec_ublock_ptr` go
   away, `VM_MOVE` becomes a node move), and the `g_ublocks` global
   base index; the edge struct and links, all mutation sites, the
   floor-lookup `umem_view_locked`, `shares_replay` → pending drain.
   No ABI change yet; QEMU green.
3. Wrapper + predicate + per-kind gathers; `SYS_VM_SHARERS` /
   `SYS_VM_BLOCKS` / `SYS_VM_VIEWS` / `SYS_THREADS` /
   `SYS_PROC_CHILDREN` (self only at this step),
   `SYS_THREAD_DESTROY(pid, tid)`, and the `VM_DROPSHARE` pid
   argument (`llrb_tid_thread` and children-tree conversions ride
   along, as do the split raw/live pid lookup APIs and keeping zombies
   in the pid registry until final destroy). Syscall table grows in its
   ability groups (renumber follows
   the ability-grouping rule of futex-design; numbers assigned at
   implementation).
4. Destruction change: rename `SYS_PROC_REAP`/`REAP_*` to
   `SYS_PROC_DESTROY`/`PROC_DESTROY_*`; make it exact-target and allow
   any dead descendant only through an all-dead path; delete the
   block-free, view-revocation, TCB-cull, and
   irq steps. The irq deletion rides driver-side fusion, landing
   here: fused `KIRQ_MSI` (allocate+bind on the calling ring under the
   shared wildcard token), `KIRQ_BIND` deleted, `KIRQ_MSI_ADDR` added,
   release/destroy freeing `present` routes, `{slot, generation}` in
   concrete route grants; pcid and nvmed adopt the shortened setup
   protocol (pci-design §7–§8: tokens ride `QUEUES_READY`). Grant
   decoupling rides along: `creator`/creator-list fields,
   `created_grants`, and `cap_reap_one_locked` deleted; bearer
   `KCAP_REVOKE`; no quota layer. Gate the final step on "owns
   nothing, views nothing, has no children, no TCBs" with
   `SYSERR_EXIST`; extend the predicate to reapers. Centralize TCB
   completion disposal so every exit/cull path drops `thread_pins`
   exactly once. Teach init (and
   tests/hello harness) the full choreography.
   **`iommu_reap_one_locked` stays through this step** — deleting it
   before the DMA verbs exist would strand DMA-pinned zombie blocks.
5. IOMMU: domain ids, `llrb_domid_map` block-side index (doubly-linked
   domain side + back-pointer), `dma_pins` deletion,
   `SYS_VM_DMA_MAPS` / `SYS_VM_DMA_REVOKE`, force-detach in the
   endpoint destroy op — and only now delete `iommu_reap_one_locked`.
6. Opportunistic: pending idiom for irq/iommu replays; grant inline
   (with capability work).

Steps 1–3 are independently landable; 4 changes observable teardown
semantics and lands with its test updates; 5 must follow 3 (the verbs)
and carries the iommu reap deletion so no intermediate system is
unreapable.
