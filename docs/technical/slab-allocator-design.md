# Per-CPU slab allocator

Status: **planned 2026-07-27.** This adds typed, fixed-size kernel object
caches above the existing page-granularity buddy allocator. It does not replace
the buddy allocator, change user-memory ownership, or turn `malloc` into a
general size-class heap. The initial consumers are the kernel structures whose
sizes are known at compile time and whose current allocation wastes most of a
4 KiB buddy page.

The design uses mimalloc-style, per-slab sharded free lists. Each slab has an
owner CPU and three object lists:

- `free`: allocation-ready objects, touched only by the owner;
- `local_free`: objects freed by the owner, also non-atomic; and
- `thread_free`: objects freed by other CPUs, published with a Treiber-stack
  CAS.

The common local allocation and free paths perform no shared atomic operation
and take no lock. Remote free contention is distributed across slab pages
rather than concentrated in one cache-wide or CPU-wide inbox.

## 1. Goals and non-goals

### Goals

- Pack many fixed-size kernel objects into each buddy page.
- Make same-CPU allocation and free lockless with respect to other CPUs.
- Keep the local fast path free of atomic read-modify-write operations.
- Avoid allocator metadata cache-line bouncing between an owner and remote
  freeing CPUs.
- Permit frees from a CPU other than the allocating CPU.
- Preserve bounded object ownership: a remotely freed object returns to its
  slab's owner rather than becoming stranded in the freeing CPU's arena.
- Allow completely empty slabs to return safely to the buddy allocator.
- Remain usable in syscall and ordinary interrupt context under the kernel's
  existing IRQ-depth rules.
- Retain the buddy allocator as the sole authority for backing pages.

### Non-goals

- Replacing variable-sized `malloc`, `calloc`, `realloc`, and `free`.
- Slabifying page tables, stacks, user blocks, vector backing arrays, XSAVE
  buffers, or other page-sized/variable-sized allocations.
- Supporting CPU hot-unplug in the first implementation.
- Supporting allocation or free from NMI context.
- Providing constructors, destructors, NUMA placement, or cache merging.
- Supporting multi-page slabs initially.

## 2. Why typed caches

The kernel knows the relevant structure sizes at compile time, so allocation
sites name a cache explicitly:

```c
thread *t = slab_zalloc(&g_thread_cache);
slab_free(&g_thread_cache, t);
```

Likely initial caches are:

- `thread`;
- `struct process`;
- `struct ublock`;
- `struct ring`;
- capability grants; and
- IOMMU device, domain, and mapping metadata.

Typed frees avoid needing a global per-frame type table. That matters because
the memory design deliberately has no per-frame metadata array. They also make
cross-cache frees detectable in debug builds and leave existing page-aligned
`malloc` users unchanged.

An object cache is permanent after initialization. Its descriptor is
read-only:

```c
enum slab_cache_flags {
  SLAB_CACHELINE_ALIGN = 1u << 0,
};

struct slab_cache {
  const char *name;
  uint32_t id;
  uint32_t object_size;
  uint32_t stride;
  uint32_t object_offset;
  uint16_t capacity;
  uint16_t flags;
};
```

Cache registration is explicit rather than linker-section magic. `slab_init`
initializes a fixed descriptor table after the CPU-state table exists and
before normal SMP execution begins.

## 3. Existing kernel properties used by the design

Kernel paths are not preempted. `irq_disable()` additionally prevents
same-CPU interrupt re-entry and pins the caller while it reads and modifies its
per-CPU arena. Its depth counter already nests across syscall entry, interrupt
entry, and spinlocks.

Every local slab operation therefore begins by disabling IRQs *before* looking
up `cpu_state_this()`, and balances that level before returning. No lock is
needed for data touched only by the owner CPU.

The buddy allocator remains protected by `g_allocator_lock`. A slab refill or
reclamation reaches it only after leaving the local fast path. Taking the
buddy lock while the slab operation has an IRQ-disable level is legal:
`spinlock_lock` adds and later removes a nested level.

NMI handlers must not allocate or free slab objects because masking IRQs does
not serialize an NMI against the interrupted CPU-local operation.

## 4. Physical layout

The first implementation uses exactly one order-0 buddy page per slab.
Consequently, an object can recover its slab header by masking its address:

```c
struct slab *slab = (void *)((uintptr_t)obj & ~(PAGE_SIZE - 1));
```

The page is divided into two metadata cache lines followed by objects:

```text
4 KiB slab page
┌──────────────────────────────────────┐
│ owner-only metadata cache line       │
│ free, local_free, used, owner links  │
├──────────────────────────────────────┤
│ remote/immutable metadata cache line │
│ thread_free, owner CPU, cache        │
├──────────────────────────────────────┤
│ object 0                             │
├──────────────────────────────────────┤
│ object 1                             │
├──────────────────────────────────────┤
│ ...                                  │
└──────────────────────────────────────┘
```

The remotely modified atomic word must not share a line with `free`,
`local_free`, `used`, or owner list links. Otherwise every remote free would
invalidate the cache line used by local allocations.

A representative header is:

```c
struct slab {
  /* Owner CPU only. */
  void *free;
  void *local_free;
  struct slab *all_prev;
  struct slab *all_next;
  struct slab *partial_prev;
  struct slab *partial_next;
  uint16_t used;
  uint16_t capacity;
  uint32_t owner_flags;

  /* Starts on a separate cache line. */
  alignas(CACHE_LINE_SIZE) _Atomic(uintptr_t) thread_free;
  struct slab *remote_next;
  const struct slab_cache *cache;
  uint32_t owner_cpu;
  uint32_t magic;
} alignas(CACHE_LINE_SIZE);
```

The final layout may reorder fields, but compile-time assertions must prove:

- `thread_free` is on a different cache line from owner-mutated fields;
- the object region is suitably aligned;
- the header fits before the first object; and
- every slab cache holds at least two objects.

The effective object alignment and stride are:

```c
alignment = max(object_alignment, alignof(void *));
stride = align_up(max(object_size, sizeof(void *)), alignment);
```

`SLAB_CACHELINE_ALIGN` additionally raises the alignment and stride to
`CACHE_LINE_SIZE`. It is appropriate for independently modified objects such
as TCBs, where two adjacent objects sharing a line would create false sharing.
It cannot prevent false sharing between fields inside one object; that requires
padding or reorganizing the structure itself.

Objects too large to fit at least twice after the header stay on the buddy
allocator. Multi-page slabs can be considered later, but would need a reliable
header lookup from every constituent page.

## 5. Per-CPU arenas

There is one arena per CPU per cache:

```c
struct slab_arena {
  /* Owner-only line. */
  struct slab *active;
  struct slab *partial;
  struct slab *all;
  uint64_t alloc_local;
  uint64_t free_local;
  uint64_t remote_collected;
  uint64_t refill_pages;

  /* Separate line: remote producers, owner consumer. */
  alignas(CACHE_LINE_SIZE) _Atomic(struct slab *) remote_ready;
} alignas(CACHE_LINE_SIZE);
```

`struct cpu_state` holds a pointer to its row of arenas. A cache ID indexes the
row:

```c
struct slab_arena *arena =
    &cpu_state_this()->slab_arenas[cache->id];
```

The arena's owner-only and remotely modified fields occupy separate cache
lines. Global statistics are not updated on the fast path. Per-CPU statistics
may be aggregated by a slow diagnostic operation.

Slab pages are created lazily. No page is reserved merely because a cache and
CPU exist.

The arena tracks:

- one `active` slab used for allocation;
- owner-only partial slabs with collected available objects;
- all slabs owned by the arena, for diagnostics and teardown; and
- an MPSC `remote_ready` stack containing formerly full slabs that received a
  remote free.

Full slabs are absent from the allocation-visible partial list. They remain on
the arena's all-slabs list.

## 6. The three per-slab lists

All free objects use their first pointer-sized word as an intrusive link.
Object contents are no longer valid once an object is passed to `slab_free`.

### `free`

Only the owner allocates from `free`. A successful allocation is one pointer
load, one pointer store, and an owner-local `used++`.

### `local_free`

An owner-CPU free pushes onto `local_free` and performs `used--`. It does not
immediately perturb the allocation list. When `free` empties, the owner adopts
the whole local list.

The separate list is not required for mutual exclusion—the IRQ-disabled owner
already has that—but it gives allocations and frees distinct hot pointers,
batches collection, and makes all page-state transitions occur at explicit
collection points.

### `thread_free`

A remote CPU pushes onto `thread_free` using a release CAS. It does not modify
`used`, owner lists, or any owner-local counter. The owner later detaches the
whole list with an acquire operation and subtracts the number of collected
objects from `used`.

Until collection, `used` therefore conservatively counts remotely freed
objects as allocated. This is load-bearing for safe page reclamation.

## 7. Atomic remote word and slab states

The low bits of `thread_free` carry a state. Object alignment leaves at least
two low bits available:

```c
#define TF_STATE_MASK 0x3ull
#define TF_PTR_MASK   (~TF_STATE_MASK)

enum thread_free_state {
  TF_AVAILABLE = 0, /* owner will visit this active/partial slab */
  TF_FULL      = 1, /* absent from allocation lists */
  TF_QUEUED    = 2, /* remote-ready notification outstanding */
  TF_RETIRED   = 3, /* no publication is allowed */
};
```

Pointer and state must be changed by one atomic operation. A separate
`remote_pending` or `queued` boolean is not sufficient: clearing it separately
from detaching the list creates a missed-wakeup window.

The invariants are:

- `TF_AVAILABLE`: the slab is active or partial. A remote free need not notify
  the owner.
- `TF_FULL`: the remote pointer is null and the slab is not allocation-visible.
- `TF_QUEUED`: exactly one remote-ready notification owns `remote_next`.
  Further remote frees only extend the per-slab list.
- `TF_RETIRED`: the slab has no live objects and is leaving the cache. A valid
  free can never observe this state.

## 8. Allocation path

The common path is:

```c
void *slab_alloc(struct slab_cache *cache) {
  irq_disable();

  struct cpu_state *cs = cpu_state_this();
  struct slab_arena *a = &cs->slab_arenas[cache->id];
  struct slab *s = a->active;

  if (s != nullptr && s->free != nullptr) {
    void *obj = s->free;
    s->free = *(void **)obj;
    s->used++;
    a->alloc_local++;
    irq_enable();
    return obj;
  }

  void *obj = slab_alloc_slow(cache, a, cs->logical_id);
  irq_enable();
  return obj;
}
```

The local slow path still runs with IRQs disabled:

1. If the active slab has `local_free`, move it to `free`.
2. If it is `TF_AVAILABLE` with a non-null remote pointer, atomically detach
   and collect that list.
3. If the active slab is truly full, atomically change
   `(null, TF_AVAILABLE)` to `(null, TF_FULL)`. If the CAS fails, a remote free
   raced and must be collected instead.
4. Drain `remote_ready` and make those slabs partial.
5. Select an owner-visible partial slab.
6. If none exists, allocate and initialize a new buddy page.

The full transition is valid only when `free` and `local_free` are empty and
the remote pointer is null. The CAS is the linearization point. A remote free
before it makes the CAS fail; one after it observes `TF_FULL` and performs the
notification protocol below.

A fresh page is initialized entirely before becoming `active`. Its object
region is linked into `free`, `used` starts at zero, and its remote word starts
as `(null, TF_AVAILABLE)`.

`slab_zalloc` zeros exactly `object_size` bytes after the allocator has
released its IRQ-disable level. The object is not yet published, so an
interrupt allocating from the same arena during the memset is harmless.

## 9. Local free

The free path disables IRQs before determining the current CPU:

```c
void slab_free(struct slab_cache *cache, void *obj) {
  if (obj == nullptr)
    return;

  irq_disable();

  struct slab *s = slab_from_object(obj);
  slab_validate(cache, s, obj);
  uint32_t this_cpu = cpu_state_this()->logical_id;

  if (s->owner_cpu == this_cpu) {
    slab_free_local(s, obj);
    irq_enable();
    return;
  }

  slab_free_remote(s, obj);
  irq_enable();
}
```

An ordinary owner-visible slab takes the simple path:

```c
*(void **)obj = s->local_free;
s->local_free = obj;
s->used--;
```

If the slab is `TF_FULL`, the local owner must make it allocation-visible:

- attempt to change `(null, TF_FULL)` to `(null, TF_AVAILABLE)`;
- on success, insert it into the partial list;
- if the CAS observes `TF_QUEUED`, leave list insertion to remote-ready
  processing; and
- if it observes a remote pointer, collect through the ordinary state
  protocol.

A remote free racing this transition either sees `TF_FULL` and wins the
notification transition or sees `TF_AVAILABLE` and relies on the slab already
becoming owner-visible. No free is stranded.

## 10. Remote free and full-slab notification

A remote free first attempts to push the object pointer while preserving the
current state.

If the state is `TF_AVAILABLE` or `TF_QUEUED`, the successful CAS completes
the free:

```text
(old_head, state) -> (obj, state)
obj->next = old_head
```

If the state is `TF_FULL`, exactly one remote producer changes:

```text
(null, TF_FULL) -> (obj, TF_QUEUED)
```

That producer then pushes the slab header onto the owning arena's
`remote_ready` MPSC stack. IRQs remain disabled from the per-CPU lookup through
both publications, so the winning producer cannot be suspended indefinitely
between marking the slab queued and publishing its notification. The
`TF_QUEUED` state prevents reclamation during this short interval.

Further remote frees see `TF_QUEUED`, extend the slab's remote object list, and
do not enqueue another slab notification. Thus the arena-wide stack is touched
once per full-to-reusable transition, not once per object free.

The owner detaches its `remote_ready` stack with an acquire exchange. For each
slab, it CASes:

```text
(remote_head, TF_QUEUED) -> (null, TF_AVAILABLE)
```

retrying if another remote producer extends the list. It then:

1. counts and adopts the detached objects;
2. subtracts that count from `used`;
3. clears the slab's queue linkage; and
4. installs the slab as active or partial, unless it is now empty and selected
   for reclamation.

A remote free after the successful state change observes `TF_AVAILABLE`; it
need not notify because the owner is already making the slab
allocation-visible. A free before the state change is included in the
detached list. There is no missed-wakeup window.

## 11. Empty-slab retirement

The owner may reclaim a slab when its conservative `used` count becomes zero
after collection. A valid object not yet published by a remote freer still
contributes to `used`, so an in-flight remote free prevents this condition.

Retirement proceeds with IRQs disabled:

1. Adopt `local_free`.
2. Detach and account for any `thread_free` objects.
3. Verify `used == 0`.
4. CAS `(null, TF_AVAILABLE)` to `(null, TF_RETIRED)`.
5. If the CAS fails, collect the racing remote publication and reconsider.
6. Remove the slab from active, partial, and all-slabs owner lists.
7. Return the page under `g_allocator_lock`.

Once the CAS reaches `TF_RETIRED`, no valid free can reference the page:
every allocated object has already been accounted as returned. Observing
`TF_RETIRED` in `slab_free_remote` is therefore an invalid/double free and
panics in a debug kernel.

To avoid page churn, each arena/cache retains one completely empty slab as its
warm slab. Further empty slabs are returned to the buddy. This is policy, not a
correctness rule, and can be tuned from measurements.

Reclamation is opportunistic. A slab emptied entirely by remote CPUs remains
queued until its owner next enters a slow allocation, local free, or explicit
cache-drain path. The kernel has no worker thread that reclaims it
asynchronously. This delay affects memory retention, not safety or subsequent
reuse.

Slab pages are kernel-only and never have user page flags. Returning one to
the buddy therefore obeys the memory design's pristinity rule without an
address-space flush.

## 12. Memory ordering

Required ordering is intentionally narrow:

- A remote producer writes `obj->next` before a release CAS publishes `obj`.
- The owner uses an acquire CAS/exchange before reading a detached remote
  list.
- A producer that wins `TF_FULL -> TF_QUEUED` publishes the remote-ready slab
  with release ordering.
- The owner detaches `remote_ready` with acquire ordering before reading
  `remote_next` or processing the slab.
- Owner-only `free`, `local_free`, `used`, and list links are ordinary
  non-atomic fields protected by CPU ownership plus IRQ masking.

The allocator does not supply object-lifetime synchronization to its callers.
The caller must already have excluded all users before freeing an object.

## 13. Buddy interaction and failure

Only these operations take `g_allocator_lock`:

- allocating an order-0 page when an arena has no usable slab; and
- returning a retired empty slab.

No slab-wide or cache-wide lock is held while acquiring it. The existing buddy
lock order seen by callers is therefore unchanged.

If a refill cannot allocate a page, `slab_alloc` returns `nullptr`.
`slab_zalloc` preserves the same result. Individual caches may add a
panic-on-OOM wrapper where the existing call site already treats allocation
failure as fatal.

Allocating in ordinary interrupt context is legal but may reach the contended
buddy slow path. A cache that requires a hard no-refill interrupt guarantee
must maintain an explicit reserve or require allocation in process context.
Remote freeing from an interrupt remains a bounded CAS path and is the more
important interrupt use case.

## 14. Debugging and hardening

Debug builds validate on every free:

- the page magic;
- `slab->cache == cache`;
- the owner CPU is in range;
- the pointer lies in the object region;
- the pointer offset is an exact multiple of the cache stride; and
- the atomic state is not `TF_RETIRED`.

Freed object bodies are poisoned before their first word becomes a freelist
link. Allocated objects may check the poison pattern before being returned.

A debug-only per-slab allocation bitmap can diagnose double allocation and
double free. It must not be part of the release fast path: an atomic bitmap in
the slab header would cause every remote free to modify another shared
metadata line.

State transitions and list membership should be verified by a
`slab_verify_cache` routine callable from tests with all target CPUs
quiesced.

## 15. Initialization and source layout

Planned implementation:

```text
kernel/src/slab.h
kernel/src/slab.c
```

`slab.h` exposes cache descriptors and typed allocation primitives.
`slab.c` owns arena initialization, slab state transitions, refill,
reclamation, and verification.

Initialization order:

1. `allocator_init` creates the buddy allocator.
2. `cpu_state_table_init` creates the fixed CPU-state table.
3. `slab_init` allocates and attaches one arena row per CPU.
4. Per-CPU setup and normal SMP execution begin.

Early boot allocations before step 3 continue using `malloc_unlocked`.
Caches are never used before their arena rows exist.

The first implementation should use an explicit cache descriptor table. This
keeps capacity, alignment, and flags reviewable in one place and avoids
freestanding linker-registration machinery.

## 16. Implementation sequence

1. Implement cache sizing, page layout, per-CPU arena initialization, and
   same-CPU allocation/free.
2. Add the atomic `thread_free` pointer without full-slab removal; stress
   remote push and owner collection.
3. Add `TF_FULL`, `TF_QUEUED`, and the remote-ready notification protocol.
4. Add `TF_RETIRED` and empty-page return to the buddy.
5. Convert `thread` and `process`, retaining their current zero-allocation
   semantics.
6. Convert `ublock`, `ring`, capability, and IOMMU metadata one cache at a
   time.
7. Add debug poisoning, cache verification, and counters.
8. Measure remote CAS retries, page occupancy, buddy refills, retained warm
   slabs, and cache-line-alignment costs before adding further policy.

## 17. Required tests

- Fill and drain a slab entirely on its owner CPU.
- Interleave owner allocation and local free with nested IRQ-disable levels.
- Have every other CPU remotely free objects from one slab and verify that
  each object is collected exactly once.
- Remotely free into a full slab while its owner concurrently attempts the
  `TF_AVAILABLE -> TF_FULL` transition.
- Race additional remote frees with `TF_QUEUED -> TF_AVAILABLE` collection
  and verify that no object is stranded.
- Make the last live objects remote frees and verify that reclamation cannot
  precede their publication or accounting.
- Repeatedly retire and reuse the same physical buddy page to exercise ABA-like
  address reuse in the remote-ready stack.
- Exhaust the buddy allocator during refill and verify a clean `nullptr`
  result without corrupting the active slab.
- Verify cross-cache, misaligned, duplicate, and post-retirement frees are
  diagnosed in debug builds.
- Compare buddy page consumption before and after converting each fixed-size
  type.

## 18. Deferred extensions

- Batch a chain of remote objects in one CAS if measurements show heavy
  contention on a single slab's `thread_free`.
- Add per-cache reserves for allocation in hard interrupt paths.
- Add multi-page slabs for structures too large to pack efficiently in one
  page.
- Route general small `malloc` sizes through slab caches only if a reliable,
  explicit allocation-kind scheme is introduced; typed caches do not depend
  on this.
- Add CPU-offline draining and slab ownership transfer if CPU hot-unplug
  becomes a kernel feature.
