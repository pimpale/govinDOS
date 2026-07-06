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
- **Multiuser**: per-uid accounting and no cross-process information leaks.

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
scheduler stack first. Planned preemption preserves it: a timer IRQ during a
kthread stays on that stack with `g_as_kernel` current; during user code it
takes the per-CPU stack. Assert it cheaply at kthread dispatch:
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
  vec_process_ptr sharers;   // processes other than owner with a view
  struct spinlock lock;
};

struct process {
  ...
  struct spinlock mem_lock;
  vec_ublock_ptr  blocks;     // owned
  vec_ublock_ptr  shared_in;  // shared to me by others
};
```

The `ublock` list is the **sole authority** on ownership and lifetime; the
page trees remain the authority for access checks (`user_range_ok` is
unchanged). **This retires `g_frames` entirely.** Its one load-bearing job
today is `umem_free`'s "is this range `FRAME_USER` owned by this pid" check,
and an exact-match ublock lookup (base must name a block the caller owns) is a
strictly stronger version of it — it also rejects freeing a sub-range or a
misaligned base, which per-frame tags can't. Everything else in `g_frames` is
debug tagging reconstructible from the buddy + ublock lists + page trees.
Per-frame metadata earns its keep in kernels with fork-style CoW, swap, or a
file page cache — none of which fit an identity-mapped SASOS. (One caveat:
the shared-subtree representation in Appendix A wants a one-word-per-frame
refcount for page-table pages; if that lands, that single field returns.)

### Syscalls

| call | semantics |
|---|---|
| `SYS_MEM_ALLOC(order, prot)` | buddy-alloc 2^order pages, **zero them**, `as_flag(p->as, …, prot\|PAGE_U)` + flush, return base |
| `SYS_MEM_FLAG(base, len, prot)` | sub-range re-flag within a block the caller has a view of; applied to the **caller's AS only** |
| `SYS_MEM_SHARE(base, pid, prot)` | owner only: map whole block `prot\|PAGE_U` into target's AS, add to `sharers` |
| `SYS_MEM_UNSHARE(base[, pid])` | sharer drops its view / owner revokes one sharer: re-flag pristine in that AS + flush |
| `SYS_MEM_FREE(base)` | owner only: full teardown (below) |

Notes:

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

### Free path (also the owner-exit path per block)

1. Lock the ublock. For owner's AS and each sharer's AS:
   `as_flag(as, base, end, PAGE_KERNEL_PRISTINE)` + `as_flush(as)`.
2. `buddy_page_free`.
3. Unlink from owner's `blocks` and every sharer's `shared_in`.

Step 1-before-2 ordering is the pristinity/TLB rule from §2.

## 6. Owner-death policy for shared blocks: **revoke** (option 1)

Recommendation: **the creator is the ultimate owner; when it dies, sharers'
access is revoked** (their views are re-flagged pristine, so a subsequent
ring-3 touch takes a page fault → kill or error delivery). Rationale:

- **Deterministic reclamation.** Block lifetime == owner lifetime. A leaked or
  hostile share can never pin memory; when a user's last process dies, all
  memory charged to that user is provably back in the buddy.
- **Crisp multiuser accounting.** The charge stays on the creator's uid for
  the block's whole life (§8). Survivorship (option 2) forces a re-charge to
  the inheriting process's uid at owner death, including a "bequest would
  exceed the heir's quota" failure path — policy sludge with no payoff at this
  stage.
- **Mechanically free.** Owner exit walks `blocks` and runs the §5 free path;
  revocation of sharers *is* step 1. Option 2 needs extra transfer logic.
- Sharing is cooperative — a sharer must already handle the owner
  disappearing; a clean fault is a better failure than silently keeping a page
  whose logical owner is gone.

The `ublock` structure (explicit owner + sharer list) keeps option 2 available
as a later per-share flag (`SHARE_BEQUEATH`: on owner death, promote the first
sharer to owner and transfer the uid charge) without reshaping anything.

## 7. Teardown paths

### Process exit (last thread dead)

1. For each `shared_in` block: remove `p` from its sharer list (its AS is
   dying anyway; this keeps sharer lists truthful).
2. For each owned block: §5 free path (revokes sharers, restores pristine,
   buddy-frees).
3. Ring/TCB teardown (existing loose end; ring pages are user memory and fall
   out of step 2 once rings allocate from ublocks).
4. `as_list_unregister(p->as)`; wait until no `g_cpu_state_table[i].current_as
   == p->as`. This terminates because of the §3 rule: every CPU switches to
   `g_as_kernel` (idle/kthread) or another process's AS at its next dispatch;
   an idle CPU never sits in HLT holding a dead CR3. Waiting runs in the
   reaper kthread with IRQs on, so it may simply poll.
5. `as_free(p->as)` — returns all its page-table frames to the buddy (already
   implemented).

Steps 4–5 live in a **reaper kthread**, which also owns dead-*thread* reaping:

### Kernel thread death

On reap of a dead kthread:

1. `as_flag(g_as_kernel, stack_base, stack_base + PAGE_SIZE,
   PAGE_KERNEL_PRISTINE)` + flush — the guard was only ever punched there
   (§3 guard scoping); the merge pass folds the PT back into the hugepage.
2. `free(stack_base)` where `stack_base = stack_top - KERNEL_TASK_STACK_SIZE`.
3. Free the TCB.

User threads are stackless in the kernel; their death costs nothing here.

## 8. Multiuser

- `struct user_account { uint64_t uid; _Atomic uint64_t bytes; uint64_t limit; }`,
  looked up/created at `process_create_user`. `SYS_MEM_ALLOC` charges the
  owner's uid and fails with `-ENOMEM`-equivalent past the limit;
  free/owner-exit uncharges. Under the revoke policy the charge never
  migrates.
- Permission checks are ublock checks: only the owner may free or share; a
  sharer may only flag/unshare its own view.
- Zero-on-alloc (§5) is the isolation half of multiuser; the page trees are
  the enforcement half.

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
per-AS lock, `as_table_count`), umem.c/h (ublocks, registry, uid
accounts), reaper.c/h (thread/process teardown, AS drain), ring.c
(`ring_destroy`), stacks.c (`stacks_free_kernel`), interrupts.c (ring-3
faults kill the process). Boot self-tests in init.c cover split→merge
shape restoration and two-process share/protect/revoke; userspace/hello.c
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
8. **uid accounting + zero-on-alloc** (zeroing can land with 6).

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
