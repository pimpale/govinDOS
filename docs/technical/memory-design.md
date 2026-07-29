# Memory design: paging, allocation, sharing, teardown

Design for user + kernel memory management on top of what exists today:
identity-mapped SASOS (VA == PA everywhere), one buddy allocator as the single
source of both physical frames and virtual placement, `as_flag`-based page-tree
manipulation, per-frame metadata in `g_frames`.

## 1. Goals

- **Isolation despite SASOS**: all processes see the same address *layout*, but
  each process gets its own hardware page tree so it can only touch memory
  granted to it. Same VA==PA convention everywhere; a shared pointer is valid
  in every AS that maps it.
- **Deterministic cleanup**: process exit unmaps and reclaims everything it
  owned; kthread death reclaims its stack and removes the guard page; no flag
  applied by a process survives the memory's return to the allocator.
- **Sharing** between user processes, with a defined owner-death policy.
- **Continuous merging**: when flag fragmentation is reverted, the page tree
  collapses back to hugepages.
- **Isolation**: per-process views and zero-on-allocation prevent
  cross-process information leaks; account policy stays in userspace.

## 2. The core invariant: the buddy only holds pristine memory

Everything else in this design hangs off one rule:

> **Pristinity invariant.** Any block returned to the buddy allocator must be
> mapped identically to boot state — present, `PAGE_R|PAGE_W|PAGE_X`, U=0,
> write-back — **in every live address space**, before `free()` runs.
> Conversely, every allocation may assume it received pristine memory.

Define this once:

```c
#define PAGE_KERNEL_PRISTINE (PAGE_R | PAGE_W | PAGE_X)   // boot identity flags
```

Consequences:

- Freeing a user block = `as_flag(as, base, end, PAGE_KERNEL_PRISTINE)` in each
  AS that had a view of it. Not "unmap": restoring the boot mapping is what
  lets the merge pass (§4) collapse the region back into the surrounding
  kernel hugepages, and keeps kernel `malloc` users of recycled memory working.
- Killing a guard page = re-flag it `PAGE_KERNEL_PRISTINE` in every AS it was
  punched in — which §3's guard scoping arranges to be `g_as_kernel` alone for
  kthread stacks — before the stack's `free()`. Today `stacks_alloc_kernel`
  punches the guard hole but nothing restores it — with recycling, a later
  `malloc` would hand out memory containing a non-present page.
- Because user sub-range flags (§5) only ever fragment the *page tree*, and a
  whole-block `as_flag` back to pristine overwrites every leaf and frees the
  sub-tables (`install_leaf` already frees finer subtrees when installing a
  coarser leaf), **flag cleanup is structural — the kernel never enumerates or
  trusts what the process did to its pages**. One call erases all of it.
- TLB ordering is part of the invariant: unmap/re-flag → `as_flush` (all
  affected ASes) → *then* `free()`. A stale TLB entry on another CPU pointing
  at a reallocated frame is a cross-process write primitive.

## 3. Address spaces: one hardware tree per process, kernel skeleton in all

### Structure

- `g_as_kernel`: the identity map, as today — the AS kthreads and the idle
  loop run on, and the only AS that ever carries kthread-stack guard punches.
- `g_as_template`: a frozen snapshot of the kernel skeleton, sealed at the end
  of boot (after per-CPU stacks + their static guards, before any user
  process). Never mutated afterwards; exists only to be cloned.
- Each user process: `p->as = as_clone(g_as_template)` at creation (replacing
  today's `p->as = g_as_kernel`). The clone contains the full kernel mapping
  with U=0 — required because syscalls/interrupts run on the user CR3 with the
  single per-CPU kernel stack; there is no CR3 switch on kernel entry.
  Ring-3 isolation comes purely from which leaves carry `PAGE_U`, and a fresh
  clone carries none.

Why clone the template and not live `g_as_kernel`: a clone taken while a
kthread is alive would copy its guard punch; the restore at that kthread's
death hits `g_as_kernel` only (see below), leaving the clone with a stale
non-present page. Once the buddy recycles that stack, `SYS_MEM_ALLOC` can
hand the process a block with a hole in its own AS — a delayed pristinity
violation. The frozen template makes "user clones never contain dynamic
guards" true by construction.
- User blocks granted to a process get `PAGE_U` leaves **only in that
  process's tree** (and sharers'). In every other AS the same range simply
  stays kernel-RWX — invisible to other ring-3 code, still usable by the
  (trusted) kernel.

Why not share kernel sub-trees across ASes (Linux-style split PML4): in an
identity-mapped SASOS the heap is one range — user blocks and kernel data
interleave at 4 KiB granularity inside the same GiB regions, so there is no
PML4/PDPT boundary to share at. Deep clones are cheap at our scale (tree size
is proportional to fragmentation, which §4 actively minimizes). Revisit with
refcounted shared subtrees + PCID if clone cost ever matters.

### Keeping kernel mappings coherent: guard scoping by stack type

The key observation: **all post-boot kernel-mapping churn is kthread-stack
guard pages, and kthread stacks are only ever touched while `g_as_kernel` is
current** (rule below). Syscalls and interrupts run on the per-CPU kernel and
interrupt stacks, whose guards are punched at boot — before the template is
sealed — and are therefore in every clone already. So:

- `STACK_TYPE_KERNEL_INTERRUPT` / `BOOTSTRAP` guards: punched pre-seal, land
  in `g_as_kernel` *and* `g_as_template` (and thus every future clone). These
  must be everywhere because syscalls run on user CR3.
- `STACK_TYPE_KERNEL_TASK` guards: punched in **`g_as_kernel` only**, at
  spawn; restored in `g_as_kernel` only, at reap. No other AS ever diverges,
  so nothing needs restoring there — the pristinity invariant (§2) holds in
  all ASes with single-AS operations.

No broadcast machinery is needed on any hot path — and none was built:
`kas_flag` was dropped from the design entirely. If a post-boot change ever
genuinely must appear everywhere (e.g. remapping a range UC/WC for a
hot-plugged device — cache-attribute aliasing across trees is a correctness
bug), a loop over a registered-AS list can be added then. Until such a
driver exists, the rule is simply that post-seal kernel-mapping mutations
target only `g_as_kernel` (today's only mutators: stacks.c guard punches
and umem.c, which touches user-process ASes).

### Rule (load-bearing): kthread stacks imply `g_as_kernel`

The scheduler switches CR3 to `proc->as` when dispatching a user thread, and
to `g_as_kernel` when dispatching a kthread or entering the idle HLT loop.
This gives process-exit teardown a clean termination condition (§7), drains
dying ASes off CPUs naturally — and is what makes guard scoping sound: RSP
may only point into a kthread stack while `g_as_kernel` is current.
`arch_thread_install` (CR3 switch) already runs before `switch_context` moves
RSP onto the kthread stack, and the symmetric path back lands on the per-CPU
scheduler stack first. Preemption (implemented 2026-07-08) preserves it: the
quantum timer can only fire in ring 3 (the kernel runs IRQs-off), and its
handler takes the per-CPU interrupt stack like any other ring-3 entry. Assert it cheaply at kthread dispatch:
`g_cpu_state_table[me].current_as == g_as_kernel` — if this rule is ever
violated, stack-overflow detection silently disappears, so it must be loud.

## 4. Hugepage merging (revert ⇒ re-merge)

Today `install_leaf` splits hugies on demand but never merges, so one guard
page permanently costs a PT + PD and 4 KiB TLB granularity for that GiB.

Add the inverse pass in `paging.c`:

- After `as_flag`'s install loop, call `try_merge_path(as, addr)` and
  `try_merge_path(as, end - 1)`. Only boundary tables can have become
  homogeneous — interior tables were fully overwritten with maximal leaves
  already — so at most 2 paths × 3 levels need checking.
- `try_merge_path` walks from level 1 upward. A table merges into its parent
  slot when all 512 entries are:
  - **all absent** → parent slot := 0, `pt_free(table)`; or
  - **all present leaves with identical flag bits and contiguous identity
    translation** (`entry[i].addr == parent_base + i*step`, PS set where the
    level requires) → parent slot := huge leaf, `pt_free(table)`.
  Then recurse upward (PT→2 MiB, PD→1 GiB).
- Merging (like splitting) preserves translation and permissions but changes
  TLB granularity: `mark_dirty` over the merged span, existing flush handles it.

Because a freed block is restored to `PAGE_KERNEL_PRISTINE` — exactly the boot
identity flags — a free in *any* AS merges all the way back into the 1 GiB
hugepage it came out of. The 512-entry scans are trivially cheap relative to
the `as_flag` that triggered them.

## 5. User memory: power-of-two blocks (`ublock`)

The user-facing allocation unit is a buddy block, which makes the power-of-two
requirement native. Replace `umem_alloc`'s `calloc` with direct
`buddy_page_alloc` so base/order are exact and recoverable.

```c
// umem.h (grows out of uaccess.c's umem_* half)
struct ublock {
  uint64_t        base;      // identity VA == PA, buddy-aligned
  uint8_t         order;     // size = PAGE_SIZE << order
  struct process *owner;     // never null while the block lives
  llrb_pid_edge  *sharers;   // owning pid -> inline share-edge tree
  llrb_domid_map *dma_maps;  // owning domain-id -> inline DMA-map tree
};

struct process {
  ...
  struct svclock   ulock;
  llrb_base_block *blocks;    // owning tree; ublocks are inline values
  llrb_base_edge  *views;     // secondary index into share edges
};
```

The process block tree physically owns each ublock node. `g_ublocks` is a
secondary base-to-pointer index into those stable inline values. `VM_MOVE`
extracts and relinks the same owning node, so both the ublock address and the
global secondary entry remain stable.

### Locking as implemented (2026-07-11)

The first implementation used one global umem lock for everything.
That was split (same day, after profiling the design on paper) into a
two-level control-plane hierarchy, acquired strictly top-down. Both levels
are `svclock`s — spinlocks whose waiters keep servicing TLB shootdowns —
because each can be held across `as_flush` by some path:

1. **`g_umem`** — the control-plane lock: every ownership-graph
   mutation (free, share, unshare, move, destroy, kill, the pid registry)
   and all kernel-ring/group bookkeeping (ipc-process-design.md §7).
   Blocks are freed only under it, so holding it pins every ublock.
2. **`p->ulock`** — sole guard of `p->blocks`/`p->views`, reads
   included. Finding a block in an index you hold pins it: a freer must
   unlink it first. `SYS_FUTEX_WAIT` holds it across its
   user-word load and `umem_protect` flags under it, which is what
   makes a concurrent guard (`prot == 0`) unable to yank a mapping
   between wait's access check and its kernel load.
Kernel rings separately embed a CQ lock. IRQ routes and IOMMU fault routes
pin the ring while borrowed-context posts hold that lock; endpoint teardown
detaches those routes and crosses the CQ lock before freeing it. Enumeration
copy-out needs no third lock: `g_umem` pins the output ublock against free,
and the caller's `ulock` excludes protection and view changes through the
bounded copy.

Consequences: the IPC data plane never touches `g_umem`; with the
global `g_ublocks` index added by enumeration-design, `umem_alloc`
takes `g_umem` only long enough to publish the new base-index entry
(the buddy has its own lock); and revocation's
flush round runs with *no* locks held (free path below).

The process-owned `llrb_base_block` is the **sole authority** on ownership and lifetime; the
page trees remain the authority for access checks (`user_range_ok` is
unchanged). **This retires `g_frames` entirely.** Its one load-bearing job
today is `umem_free`'s "is this range `FRAME_USER` owned by this pid" check,
and an exact-match ublock lookup (base must name a block the caller owns) is a
strictly stronger version of it — it also rejects freeing a sub-range or a
misaligned base, which per-frame tags can't. Everything else in `g_frames` is
debug tagging reconstructible from the buddy + ublock trees + page trees.
Per-frame metadata earns its keep in kernels with fork-style CoW, swap, or a
file page cache — none of which fit an identity-mapped SASOS. (One caveat:
the shared-subtree representation in Appendix A wants a one-word-per-frame
refcount for page-table pages; if that lands, that single field returns.)

### Syscalls

| call | semantics |
|---|---|
| `SYS_VM_ALLOC(len, prot)` | buddy-allocate the rounded power-of-two block, **zero it**, map `prot\|PAGE_U`, return base |
| `SYS_VM_PROTECT(base, len, prot)` | re-flag a sub-range in the caller's own view |
| `SYS_VM_SHARE(base, pid, prot)` | live owner maps the whole block into a live target and creates one edge |
| `SYS_VM_DROPSHARE(base, pid)` | viewer, or its authorized reaper, removes that incoming edge |
| `SYS_VM_UNSHARE(base, pid)` | owner, or its authorized reaper, removes one exact viewer edge |
| `SYS_VM_FREE(base)` | owner or authorized reaper; refuses while any retention remains |

Notes:

- **Two unshare verbs, split by actor (2026-07-26).** `DROPSHARE` is the
  viewer-side drop; `UNSHARE` is the owner-side per-edge revoke, making
  `SHARE`/`UNSHARE` a symmetric owner pair (both name a peer). Both accept
  the authorized-reaper form used for a dead descendant.
  The revoke verb is what lets `SYS_MEM_FREE` be a single transaction that
  fails while attached (§5): `VM_MOVE` transfers ownership but never removes
  a third party's edge, so without owner-side revoke a hung sharer holds
  free hostage.
- **Page-table pages must die like user frames (2026-07-26, required by
  futex-design §3).** `as_flag`'s overwrite and merge paths currently
  `free_table` during mutation, so a concurrent lock-free walker can chase a
  freed-and-recycled table page. The fix lives in the AS layer so every
  `as_flag` caller is covered: mutation detaches obsolete sub-trees onto a
  per-AS retirement list, and `as_flush` drains that list only after its
  shootdown is acknowledged — synchronous, no kernel worker. No table page is
  recycled before the shootdown completes. The futex wait path's view check
  depends on this; until it lands, that path holds `p->ulock` instead.
- **Flags are per-view.** A sharer restricting its view to `PAGE_R` or punching
  a guard sub-range (`prot == 0`) affects only its own tree. No shared flag
  state to reconcile, and each AS's fragmentation is collapsed independently
  at free time.
- The kernel sanitizes `prot`: `PAGE_U` forced on (except `prot==0` guard),
  cache type restricted to WB for now (WC later for framebuffers, allowlisted).
- Arbitrary flag depth needs no bookkeeping (§2): free never replays the
  process's flag history, it just overwrites the whole block.
- **Zero on alloc is mandatory** — blocks recycle across processes and users;
  handing out a dead process's heap is an info leak. Zeroing at alloc time
  covers every path (kernel-internal `malloc` users don't need it and skip it).

### Free path

`SYS_VM_FREE(base)` is a single bounded transaction. It **fails** while
anything is still attached to the block — sharers, DMA pins,
reflected-fault waiters — and detaching those is userspace's job,
done with bounded per-item verbs beforehand (`SYS_VM_UNSHARE(base, pid)` per
sharer, and so on). There is no kernel work queue and no waiter drain: the
kernel does not notify threads parked in a block that is being revoked, so
free has nothing to iterate. See [futex-design.md](futex-design.md) §5 for
the teardown flow and why notification moved to userspace.

`SYSERR_AGAIN` remains reserved in the ABI and libc `free()` keeps its retry
loop so a continuation can be added later without touching callers; no path
returns it today. The two candidates are noted at the end of this section.

The implemented enumeration and exact-removal surface makes each refusal
actionable:

| retention | enumeration / removal |
|---|---|
| share edges | `VM_SHARERS`, `VM_VIEWS`; `VM_UNSHARE`, `VM_DROPSHARE` |
| owned blocks | `VM_BLOCKS`; `VM_FREE` |
| DMA mappings | `VM_DMA_MAPS`; `VM_DMA_REVOKE` |

(Capability grants no longer appear here: grants never retain blocks —
[capability-design.md](capability-design.md) §6,
[enumeration-design.md](enumeration-design.md) §0.)

Reflected-fault enumeration is explicitly deferred with fault
reflection itself. It is not part of the implementable enumeration v1
while no reflected-fault waiters exist. The fault-reflection feature
must add its own enumerator/coercion contract before such waiters are
allowed to retain a block.

Each enumerator is a stateless, ordered read: return up to `cap` keys strictly
greater than `after`. See [enumeration-design.md](enumeration-design.md) §2.

Once nothing is attached, two release phases run (the flush is the
single longest thing the old global lock ever covered, so it moved outside):

1. **Under `g_umem`** (`block_release_prepare`): extract the owning block
   node, remove its `g_ublocks` entry, tear down any ring endpoint,
   re-flag the sole remaining owner view pristine, and pin that AS.
2. **With no locks held** (`umem_release_finish`): flush the owner AS,
   unpin it, return RAM to the buddy, and free the extracted owning node.

Sharers and DMA mappings have already been removed by the bounded pre-pass,
leaving the **page-table walk of a large block**
as the one unbounded step no pre-pass can remove: a 64 MB block at 4 K
granularity is ~16k PTE rewrites per view, plus a shootdown that waits for
cross-CPU acks. It is bounded by the caller's own allocation rather than by
anything an adversary controls, which is why it is accepted for now. Linux
solves the same problem with `cond_resched()` inside `zap_pte_range`; without
preemption the equivalent is a cursor and a resumable free.

Flush-before-buddy-free is the pristinity/TLB rule from §2. The pins
are what let the flush leave the lock: a concurrently-destroyed sharer's
`as_free` step returns SYSERR_AGAIN while pins remain (destroy already
has that retry shape for its thread/CPU drains), and once a zombie's
lists are empty no new pin can target its AS, so the gate clears in
bounded time.

## 6. Owner-death policy for shared blocks: parent-driven revoke

The creator remains the ultimate owner, but owner death itself changes no
mapping and performs no resource cleanup. The dead process continues to own
the block while its authorized reaper enumerates the owner's blocks and each
block's share edges. `VM_UNSHARE` or coerced `VM_DROPSHARE` restores each
viewer to the pristine kernel mapping; `VM_FREE` succeeds only after the share
and DMA edge sets are empty.

This preserves deterministic eventual reclamation without putting an
unbounded edge walk in process destruction. It also gives peers a coherent
window in which to drain a dead producer's still-live buffer. Prompt teardown
is a userspace protocol decision; the kernel supplies ordered enumeration and
exact removal. See [enumeration-design.md](enumeration-design.md) for the
authority predicate, cursor contract, and full post-order choreography.

## 7. Teardown paths

### Process exit (last thread dead)

Exit only marks the process dead and publishes its tree event. Its process
record, TCBs, owned blocks, incoming views, rings, and DMA edges remain
enumerable. An authorized ancestor performs the bounded post-order sequence in
[enumeration-design.md](enumeration-design.md) §5, then calls
`PROC_DESTROY`. The final destroy operation refuses while any child, TCB,
owned block, or incoming view remains; it frees only the empty address-space
body and process record.

`PROC_DESTROY` returns `SYSERR_AGAIN` while an AS pin or CPU reference is
draining and `SYSERR_EXIST` when userspace-visible resources remain. There are
no kernel reaper threads; user threads are stackless in the kernel, and exact
TCB disposal is exposed as `THREAD_DESTROY`.

## 8. Resource policy

- The kernel has no identity/account table. Login identities and POSIX
  credentials belong to userspace services.
- `SYS_VM_ALLOC` currently fails only when the global buddy allocator is out
  of memory. A later capability design may add explicit resource-budget
  objects without coupling allocation to user identities.
- `SYS_VM_SIZE(base)` returns the usable byte capacity of an exact block base
  owned by the caller. This is the rounded ublock size, not the caller's
  original requested length; a non-base or non-owned address returns
  `SYSERR_PERM`.
- Permission checks resolve through ublocks: a live owner controls sharing,
  and an authorized all-dead-path ancestor may enumerate and dismantle a
  dead descendant's exact block or view.
- Zero-on-alloc (§5) and per-process page trees enforce isolation.

## 9. Locking

- **Lock order:** `process.mem_lock` → `ublock.lock` → AS-list lock (only if
  the rare broadcast path of §3 exists) → per-AS lock → `g_allocator_lock`
  (taken inside malloc/free/pt_alloc). `g_as_template` is frozen after seal,
  so cloning it needs no lock at all.
- Add a **per-AS spinlock** guarding `as_flag`/`as_flush`/`as_clone(src)` —
  today two threads of one process calling umem syscalls concurrently would
  corrupt the tree.
- **Shootdown rule (existing, now load-bearing):** any IRQs-off spin that can
  be a shootdown target must poll `tlb_service_local()`. The per-AS lock and
  AS-list lock acquisitions happen on paths that later initiate shootdowns, so
  their spin loops must poll — add a `spinlock_lock_tlbpoll()` variant rather
  than sprinkling it ad hoc.

## 10. Implementation order

**Status: all of §§1–9 are implemented and QEMU-verified (2026-07-06),**
minus `kas_flag` (omitted per above). Key files: paging.c (merge pass,
per-AS lock, `as_table_count`), umem.c/h (ublocks and the process registry),
reaper.c/h (thread/process teardown, AS drain), ring.c
(`ring_destroy`), stacks.c (`stacks_free_kernel`), interrupts.c (ring-3
faults kill the process). Boot self-tests in init.c cover split→merge
shape restoration and two-process share/protect/revoke; packages/tests/tests.c
covers the syscall surface (`SYS_VM_PROTECT`/`SHARE`/`UNSHARE` are 11–13).

The order the work landed in, each step keeping the userspace test green:

1. **Merge pass** in `paging.c` (+ `PAGE_KERNEL_PRISTINE`, a tree-shape debug
   dump to assert "guard add/remove returns tree to boot shape").
2. **Pristinity on existing frees**: `umem_free` restores RWX (today it drops
   X); stack reaper (guard restore + free) hung off dead-thread reaping.
3. **Per-AS lock** with the tlb-polling acquire.
4. **Template seal + guard scoping**: `g_as_template` snapshot at end of
   boot; `stacks.c` punches `KERNEL_TASK` guards in `g_as_kernel` only;
   assert against post-seal mutations of any other kernel mapping.
5. **Per-process AS**: `as_clone(g_as_template)` in `process_create_user`,
   scheduler CR3 policy (user → `proc->as`, kthread/idle → `g_as_kernel`)
   with the kthread-dispatch assert, reaper-side AS drain + `as_free`.
6. **ublock layer + syscalls** (alloc/flag/free), rings allocated from
   ublocks; `g_frames` retired.
7. **Share/unshare + revoke-on-exit.**
8. **zero-on-alloc** (can land with 6).

## Appendix A: refcounted shared subtrees (persistent page trees)

Alternative internal representation for §3 — same external API
(`as_clone` / `as_flag` / `kas_flag` / `as_free`), different tree ownership.
Instead of deep-cloning the kernel tree per process and broadcasting kernel
changes into N copies, ASes share page-table pages and diverge lazily,
persistent-data-structure style (path copying, like a persistent trie).

### Core invariant: frozen when shared

Every page-table page carries a refcount = number of parent entries, across
all ASes, pointing at it. Roots are always private (rc 1).

- **rc == 1** → private: mutate in place, exactly as today.
- **rc > 1** → frozen: to mutate on behalf of one AS, **privatize** first —
  copy the 4 KiB table, `rc++` each present child, swap the copy into the
  parent slot (private by induction: the walk privatizes top-down), `rc--`
  the original.

### Operations

- **`as_clone`**: allocate a root, memcpy the 512 entries, `rc++` each present
  child. O(1) pages instead of O(fragmentation) — process creation stops
  scaling with tree shape.
- **`as_flag`**: the walk in `install_leaf` privatizes every rc>1 table on the
  path down; below that, existing logic runs unchanged (`split_huge` children
  are born rc 1). Sharing a ublock still yields per-AS views: installing
  `PAGE_U` leaves privatizes that AS's path, so §5's per-view flags fall out
  automatically.
- **`kas_flag`** (the rare everywhere-broadcast, §3): a change wanted in
  *every* AS may mutate shared tables **in place** — and cheaply: walk every
  registered root, skipping tables already visited this call (dedup by table
  PA). A region no process has touched updates one physical table, once, for
  all ASes. Guard scoping (§3) makes this path rarer still, and it makes the
  `g_as_kernel` / `g_as_template` split nearly free: the two share every
  table except along punched paths.
- **`free_table` → `table_deref`**: `rc--`; on zero, deref children and free
  the page. `as_free` = deref the root's children (precondition: the AS is
  drained off all CPUs, §7 — a CR3 switch flushes that CPU's cached walks).
- **Merge (§4)** stays per-parent and works with shared children: the
  homogeneity check is read-only, and replacing *my* parent slot with the
  equivalent huge leaf + deref'ing the child doesn't disturb other sharers.

### Refcount storage

No room in the table page itself (512×8 fills it). The natural home is a
one-word-per-frame array — the lone survivor of `g_frames`:
`uint32_t g_pt_rc[]` indexed by PFN, meaningful only for page-table frames
(4 bytes per 4 KiB ≈ 0.1% of RAM). A side hash keyed by table PA works too if
the flat array offends.

### Table lifetime vs paging-structure caches

Privatization is translation-preserving, so it needs no flush for
*correctness of translation* — but x86 paging-structure caches on a CPU that
keeps running AS A may retain partial walks **through the old shared table**.
Content-identical, so harmless — until that table's rc hits 0 and its frame is
reused; a cached walk through freed memory is a security bug (same hazard
`split_huge` already guards with its `mark_dirty`). Rules:

- Privatization does `mark_dirty(as, covered_span)` — one call into existing
  machinery; the batch's `as_flush` evicts the stale walks. (Privatizing a
  PML4-level child dirties 512 GiB → the flush degenerates to a CR3 write on
  that AS's CPUs; fine, and it happens once per divergence point.)
- rc reaches 0 only under the paging lock (below), and every deref path
  either flushes before releasing the lock or holds the drained-AS
  precondition. No CPU can retain cached walks into a dead table.

Simpler alternative if this feels tight: never free table pages eagerly —
quarantine rc-0 tables and drain the list with one all-CPU full flush when it
grows past a threshold.

### Locking

Per-AS locks (§9) are no longer sound: two ASes can race on one shared table
— e.g. `kas_flag` mutating it in place while another AS copies it to
privatize, yielding a torn copy that misses half the kernel update. Use a
**single global paging mutation lock** (tlb-polling acquire per §9), held
across mutate + flush. Read-only walks (`as_getinfo`, `user_range_ok`) stay
lock-free as today. Fine at this scale; per-table locks or epochs if it ever
contends.

### Trade-off vs deep clones

Buys: O(1) `as_clone`; one shared kernel skeleton instead of N copies;
`kas_flag` touches each physical table once instead of once per AS;
page-table memory proportional to *divergence*, not ASes × fragmentation.
Costs: the refcount array, the frozen/privatize discipline in the walk, the
table-lifetime rule, and a global lock instead of per-AS.

Since the external API is unchanged, this is a drop-in representation swap:
ship the deep-clone version first (§10 step 5) and swap when clone cost or
broadcast cost is actually felt. Nothing in §§1–10 assumes exclusive table
ownership in a way that blocks the swap.
