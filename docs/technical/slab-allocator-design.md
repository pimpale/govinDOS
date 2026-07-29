# Per-CPU slab allocator

Status: **implemented 2026-07-27.** This adds typed, fixed-size object caches above
the existing page-granularity buddy allocator. The allocator itself is a
kernel-independent C template under `kernel/vendor/slab/`, instantiated once
per object type in the same style as `vec`, `list`, and `llrb`. It uses only
standard C headers and APIs. Each implementation instantiation supplies a
`SLAB_WHICH_CPU()` policy callback; the generated allocator uses it to select
the current arena without exposing arena arguments to consumers.

This does not replace the buddy allocator, change user-memory ownership, or
turn `malloc` into a general size-class heap. The initial consumers are the
kernel structures whose sizes are known at compile time and whose current
allocation wastes most of a 4 KiB buddy page.

The design uses mimalloc-style, per-slab sharded free lists. Each slab has an
owner arena—mapped to one CPU by the kernel—and three object lists:

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
- Keep the template independent of CPU discovery, IRQ control, kernel locks,
  page constants, and other kernel APIs.
- Make the same generated allocator usable in hosted C tests.

### Non-goals

- Replacing variable-sized `malloc`, `calloc`, `realloc`, and `free`.
- Slabifying page tables, stacks, user blocks, vector backing arrays, XSAVE
  buffers, or other page-sized/variable-sized allocations.
- Supporting CPU hot-unplug in the first implementation.
- Supporting allocation or free from NMI context.
- Providing constructors, destructors, NUMA placement, or cache merging.
- Supporting multi-page slabs initially.
- Supplying a built-in definition of the caller's current CPU or thread; every
  implementation instantiation provides that policy.
- Providing synchronization for two local contexts concurrently operating on
  the same arena; that is an embedding-policy responsibility.

## 2. Template shape and generated API

The template follows the existing declaration/implementation split. A type
header declares an instance:

```c
typedef struct thread thread;

#define SLAB_NAME thread
#define SLAB_TYPE thread
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME
```

One dedicated translation unit emits its implementation:

```c
#include "cpu_state.h"
#include "thread.h"

#define SLAB_NAME thread
#define SLAB_TYPE thread
#define SLAB_PAGE_SIZE 4096
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME
```

This generates names containing `SLAB_NAME`:

```c
bool slab_thread_init(size_t cpu_count);
thread *slab_thread_malloc(size_t size);
thread *slab_thread_zalloc(size_t size);
void slab_thread_free(thread *obj);
void slab_thread_collect(void);
bool slab_thread_destroy(void);
bool slab_thread_valid(void);
```

The generated allocator is one singleton instance for its type. `_init`
allocates a cache-line-aligned table for `cpu_count` internal arenas with
standard `aligned_alloc` and zero-initializes it; normal operations select one
by calling `SLAB_WHICH_CPU()` exactly once. Arena and slab-page representations
remain generated implementation types.

`SLAB_TYPE` may be incomplete at the declaration include because the public
surface uses it only through pointers. It must be complete when
`slab_impl.h` is included so size, alignment, and capacity can be computed.

Each slab header stores a pointer to its owner arena. The generated `_free`
selects the current arena and determines local versus remote by comparing:

```c
slab->owner == current
```

Likely initial instances are:

- `thread`;
- `process`;
- `ublock`;
- `ring`;
- capability grants; and
- IOMMU device, domain, and mapping metadata.

Typed frees avoid needing a global per-frame type table. That matters because
the memory design deliberately has no per-frame metadata array. They also make
cross-type frees detectable in debug builds and leave existing page-aligned
`malloc` users unchanged.

The declaration template requires `SLAB_NAME` and `SLAB_TYPE`. The
implementation additionally requires `SLAB_PAGE_SIZE`,
`SLAB_CACHELINE_SIZE`, and `SLAB_WHICH_CPU()`. Optional configuration follows
the `llrb` precedent:

```c
#define SLAB_OBJECT_ALIGNMENT alignof(SLAB_TYPE) /* default */
#define SLAB_CACHELINE_ALIGN  1                  /* default 0 */
#define SLAB_ALLOC_PAGE()     aligned_alloc(SLAB_PAGE_SIZE, SLAB_PAGE_SIZE)
#define SLAB_FREE_PAGE(ptr)   free(ptr)
```

The allocation hooks default to the shown standard C functions. Overrides
exist for hosted tests, fault injection, and embeddings with a different
standards-shaped allocator; govindos uses the defaults through its
freestanding `aligned_alloc`.

`_malloc(size)` deliberately retains a size argument even though the generated
instance accepts only `sizeof(SLAB_TYPE)`. A mismatch asserts in a debug build
and returns `NULL` in a release build. This lets another vendor template
delegate a typed allocation hook to either `malloc(size)` or
`slab_type_malloc(size)` without changing the hook's shape.

## 3. Portability and ownership contract

The template includes only standard C headers:

```text
assert.h
stdalign.h
stdatomic.h
stdbool.h
stddef.h
stdint.h
stdlib.h
string.h
```

It does not include `cpu_state.h`, `irq.h`, `spinlock.h`, `allocator.h`, or
`paging.h`. It does not call a CPU-ID, preemption, IRQ, scheduler, or buddy
function.

Instead, the embedding must uphold these rules:

1. `SLAB_WHICH_CPU()` returns an index less than the count passed to `_init`.
2. Its result remains stable for the entire generated operation. The callback
   selects an already-pinned context; it is not itself a pinning primitive.
3. Calls that select the same index are serialized with respect to that
   arena's owner-only state.
4. Any number of other indices may concurrently free objects owned by that
   arena; those calls use only its remote atomic state.
5. `_init` runs before concurrent use, and `_destroy` runs only after every
   operation and object has quiesced.
6. The allocation returned by `SLAB_ALLOC_PAGE` has size and alignment
   `SLAB_PAGE_SIZE`; `SLAB_PAGE_SIZE` is a power of two.

For govindos, kernel paths are not preempted and `irq_disable()` prevents
same-CPU interrupt re-entry. Syscall and ordinary interrupt paths already run
pinned with IRQs masked, and spinlock nesting preserves that condition.
The implementation instantiation therefore supplies:

```c
#define SLAB_WHICH_CPU() cpu_state_whoami()
```

The callback only selects the arena. If a future kernel permits migration or
same-CPU interrupt re-entry across allocator calls, the call sites or a higher
standard-library boundary must restore §3's pinning/serialization contract.
NMI handlers must not use the allocator because masking ordinary IRQs does not
serialize an NMI against the interrupted owner operation.

In a hosted test, `SLAB_WHICH_CPU()` may read a standard `_Thread_local` index.
No kernel shim is required.

## 4. Physical layout

The first implementation uses exactly one `SLAB_PAGE_SIZE` allocation per
slab. In govindos that is one 4 KiB order-0 buddy page. Consequently, an object
can recover its slab header by masking its address:

```c
SLAB_PAGE_T *slab =
    (void *)((uintptr_t)obj & ~(SLAB_PAGE_SIZE - 1));
```

The page is divided into two metadata cache lines followed by objects:

```text
4 KiB slab page
┌──────────────────────────────────────┐
│ owner-only metadata cache line       │
│ free, local_free, used, owner links  │
├──────────────────────────────────────┤
│ remote/immutable metadata cache line │
│ thread_free, owner arena, type tag   │
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
  /* Logical owner only. */
  void *free;
  void *local_free;
  struct slab *all_prev;
  struct slab *all_next;
  struct slab *partial_prev;
  struct slab *partial_next;
  size_t used;
  unsigned owner_flags;

  /* Starts on a separate cache line. */
  alignas(SLAB_CACHELINE_SIZE) _Atomic(uintptr_t) thread_free;
  struct slab *remote_next;
  SLAB_ARENA_T *owner;
  const void *type_tag;
} alignas(SLAB_CACHELINE_SIZE);
```

The names above are explanatory; `slab_impl.h` token-pastes private page and
arena type names from `SLAB_NAME`. Each generated implementation owns one
static type-tag object, and every page records its address. Debug validation
therefore detects calling one generated type's `_free` on another type's
object without a global registry or kernel metadata.

The final layout may reorder fields, but compile-time assertions must prove:

- page and cache-line sizes are powers of two;
- the page size is a multiple of the cache-line size;
- `thread_free` is on a different cache line from owner-mutated fields;
- the object region is suitably aligned;
- the header fits before the first object; and
- every generated instance holds at least two objects per slab.

The effective object alignment and stride are:

```c
alignment = max(SLAB_OBJECT_ALIGNMENT, alignof(void *));
stride = align_up(max(sizeof(SLAB_TYPE), sizeof(void *)), alignment);
```

`SLAB_CACHELINE_ALIGN` additionally raises the alignment and stride to
`SLAB_CACHELINE_SIZE`. It is appropriate for independently modified objects
such as TCBs, where two adjacent objects sharing a line would create false
sharing. It cannot prevent false sharing between fields inside one object;
that requires padding or reorganizing the structure itself.

Objects too large to fit at least twice after the header stay on the buddy
allocator. Multi-page slabs can be considered later, but would need a reliable
header lookup from every constituent page.

## 5. Internal arena table

The generated implementation defines an internal arena type:

```c
struct SLAB_ARENA_T {
  /* Owner-only line. */
  SLAB_PAGE_T *active;
  SLAB_PAGE_T *partial;
  SLAB_PAGE_T *all;
  SLAB_PAGE_T *warm;
  uint64_t alloc_local;
  uint64_t free_local;
  uint64_t remote_collected;
  uint64_t refill_pages;

  /* Separate line: remote producers, owner consumer. */
  alignas(SLAB_CACHELINE_SIZE) _Atomic(SLAB_PAGE_T *) remote_ready;
} alignas(SLAB_CACHELINE_SIZE);
```

This structure knows nothing about CPUs. It is simply single-owner storage
which may receive concurrent remote frees. The generated singleton holds:

```c
struct SLAB_STATE_T {
  SLAB_ARENA_T *arenas;
  size_t arena_count;
  bool initialized;
};
```

`_init(cpu_count)` allocates the table with standard `aligned_alloc` so
adjacent over-aligned arenas cannot share cache lines, then zeroes it. Each
operation calls `SLAB_WHICH_CPU()` once, bounds-checks the result in debug
builds, and uses `&arenas[index]` for the remainder of the operation. The arena
address is stable until quiescent `_destroy`.

The arena's owner-only and remotely modified fields occupy separate cache
lines. The template updates no shared global statistics. `_valid` may
aggregate owner-local counters only after the caller quiesces the instance.

Slab pages are created lazily. `_init` allocates arena metadata but no slab
page.

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
Object contents are no longer valid once an object is passed to the generated
`_free`.

### `free`

Only the owner allocates from `free`. A successful allocation is one pointer
load, one pointer store, and an owner-local `used++`.

### `local_free`

An owner-CPU free pushes onto `local_free` and performs `used--`. It does not
immediately perturb the allocation list. When `free` empties, the owner adopts
the whole local list.

The separate list is not required for mutual exclusion—the embedding already
serializes owner operations—but it gives allocations and frees distinct hot
pointers, batches collection, and makes all page-state transitions occur at
explicit collection points.

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
SLAB_TYPE *SLAB_FN(_malloc)(size_t size) {
  if (size != sizeof(SLAB_TYPE)) {
    assert(size == sizeof(SLAB_TYPE));
    return NULL;
  }
  size_t index = SLAB_WHICH_CPU();
  assert(index < SLAB_LOCAL(state).arena_count);
  SLAB_ARENA_T *a = &SLAB_LOCAL(state).arenas[index];
  SLAB_PAGE_T *s = a->active;

  if (s != NULL && s->free != NULL) {
    SLAB_TYPE *obj = s->free;
    s->free = *(void **)obj;
    s->used++;
    a->alloc_local++;
    return obj;
  }

  return SLAB_LOCAL(alloc_slow)(a);
}
```

The template assumes the embedding continues to serialize this arena while
the local slow path runs:

1. If the active slab has `local_free`, move it to `free`.
2. If it is `TF_AVAILABLE` with a non-null remote pointer, atomically detach
   and collect that list.
3. If the active slab is truly full, atomically change
   `(null, TF_AVAILABLE)` to `(null, TF_FULL)`. If the CAS fails, a remote free
   raced and must be collected instead.
4. Drain `remote_ready` and make those slabs partial.
5. Select an owner-visible partial slab.
6. If none exists, call `SLAB_ALLOC_PAGE` and initialize the returned page.

The full transition is valid only when `free` and `local_free` are empty and
the remote pointer is null. The CAS is the linearization point. A remote free
before it makes the CAS fail; one after it observes `TF_FULL` and performs the
notification protocol below.

A fresh page is initialized entirely before becoming `active`. Its owner
pointer is set to the supplied arena, its object region is linked into `free`,
`used` starts at zero, and its remote word starts as
`(null, TF_AVAILABLE)`.

The generated `_zalloc` calls `_malloc` and then standard `memset` over exactly
`sizeof(SLAB_TYPE)` bytes. It accepts and validates the same size argument.

## 9. Local free

The generated free selects the caller's current arena:

```c
void SLAB_FN(_free)(SLAB_TYPE *obj) {
  if (obj == NULL)
    return;

  size_t index = SLAB_WHICH_CPU();
  assert(index < SLAB_LOCAL(state).arena_count);
  SLAB_ARENA_T *current = &SLAB_LOCAL(state).arenas[index];
  SLAB_PAGE_T *s = SLAB_LOCAL(page_from_object)(obj);
  SLAB_LOCAL(validate)(s, obj);

  if (s->owner == current) {
    SLAB_LOCAL(free_local)(current, s, obj);
    return;
  }

  SLAB_LOCAL(free_remote)(s, obj);
}
```

The template does not interpret the callback's index as a hardware CPU ID. It
is only an arena selector governed by §3's embedding contract.

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
`remote_ready` MPSC stack. The owner arena comes directly from the immutable
pointer in the slab header. `TF_QUEUED` prevents reclamation and duplicate
notification while the two publications are in progress. The generated
`_free` completes the notification before returning; embeddings that support
asynchronous cancellation must not cancel a context inside `_free`.

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

Retirement is performed only by the serialized owner:

1. Adopt `local_free`.
2. Detach and account for any `thread_free` objects.
3. Verify `used == 0`.
4. CAS `(null, TF_AVAILABLE)` to `(null, TF_RETIRED)`.
5. If the CAS fails, collect the racing remote publication and reconsider.
6. Remove the slab from active, partial, and all-slabs owner lists.
7. Return the page through `SLAB_FREE_PAGE`.

Once the CAS reaches `TF_RETIRED`, no valid free can reference the page:
every allocated object has already been accounted as returned. Observing
`TF_RETIRED` in `slab_free_remote` is therefore an invalid/double free and
fails a standard `assert` in a debug build.

To avoid page churn, each arena/type retains one completely empty slab as its
warm slab. Further empty slabs are returned to the buddy. This is policy, not a
correctness rule, and can be tuned from measurements.

Reclamation is opportunistic. A slab emptied entirely by remote CPUs remains
queued until its owner next enters a slow allocation, local free, or explicit
cache-drain path. The kernel has no worker thread that reclaims it
asynchronously. This delay affects memory retention, not safety or subsequent
reuse.

In govindos, slab pages are kernel-only and never have user page flags.
`free()` ultimately returns the aligned page to the buddy under its existing
standard-library wrapper, obeying the memory design's pristinity rule without
an address-space flush.

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
  non-atomic fields protected by §3's single-owner serialization contract.

The allocator does not supply object-lifetime synchronization to its callers.
The caller must already have excluded all users before freeing an object.

## 13. Standard allocation boundary and failure

The template obtains backing storage with:

```c
void *page = aligned_alloc(SLAB_PAGE_SIZE, SLAB_PAGE_SIZE);
```

and returns it with standard `free(page)`. `SLAB_PAGE_SIZE` satisfies
`aligned_alloc`'s requirement that the size be a multiple of the alignment.
The template neither knows nor cares whether those functions are backed by a
hosted libc, a buddy allocator, or a test harness.

Govindos provides the C `aligned_alloc` entry point in its freestanding
`stdlib`. For a page-sized/page-aligned request it acquires
`g_allocator_lock` and delegates to the buddy allocator. Existing `free`
already performs the inverse operation. This policy remains outside
`kernel/vendor/slab`.

No template lock is held while calling `aligned_alloc` or `free`; only the
embedding's owner serialization remains active. In the kernel that means the
caller's IRQ-disable level remains nested across the standard-library call,
matching current allocator behavior.

If `aligned_alloc` returns `NULL`, the generated `_malloc` returns `NULL`
with its page/list invariants intact. `_zalloc` preserves the same result.
Call sites may panic where the existing code already treats OOM as fatal.

Allocation from ordinary kernel interrupt context is legal but may reach the
contended standard-library/buddy slow path. A type requiring a hard no-refill
interrupt guarantee must maintain an explicit reserve or require allocation
in process context. Remote freeing from an interrupt remains a bounded CAS
path and is the more important interrupt use case.

## 14. Debugging and hardening

Debug builds validate on every free:

- the slab's type tag matches the generated instance;
- the owner arena pointer is non-null;
- the pointer lies in the object region;
- the pointer offset is an exact multiple of the generated stride; and
- the atomic state is not `TF_RETIRED`.

Freed object bodies are poisoned before their first word becomes a freelist
link. Allocated objects may check the poison pattern before being returned.

A debug-only per-slab allocation bitmap can diagnose double allocation and
double free. It must not be part of the release fast path: an atomic bitmap in
the slab header would cause every remote free to modify another shared
metadata line.

State transitions and list membership are verified by the generated `_valid`
routine. Its caller must quiesce every arena and possible remote free first.

## 15. Initialization and source layout

Implemented source layout:

```text
kernel/vendor/slab/slab.h
kernel/vendor/slab/slab_impl.h
kernel/src/instances/slab_thread.c
kernel/src/instances/slab_process.c
kernel/src/instances/slab_ring.c
kernel/src/instances/llrb_{base_block,base_edge,base_ublock,pid_edge,tid_thread}.c
kernel/src/instances/slab_llrb_*.c  # older standalone instantiations
kernel/src/slabs.h
kernel/src/slabs.c
```

`vendor/slab/slab.h` generates typed declarations; arena and page types stay
private to `vendor/slab/slab_impl.h`, which generates the state transitions,
refill, reclamation, and verification. Each fixed public type has one
instances translation unit. New LLRB indices put their node slab in the same
implementation file, avoiding a second translation unit per index. Private
capability and IOMMU metadata instantiate the same template beside their
complete private structure definitions.

Each implementation instantiation defines `SLAB_WHICH_CPU()` and emits one
singleton allocator. `kernel/src/slabs.{c,h}` initializes those generated
instances; consumers call their conventional
`_malloc(size)`/`_free(ptr)` functions directly. CPU policy enters through the
instantiation macro rather than through the vendor source.

Initialization order:

1. `allocator_init` creates the buddy allocator.
2. `cpu_state_table_init` creates the fixed CPU-state table.
3. `slabs_init` calls each generated `_init(g_cpu_state_table_len)`.
4. Per-CPU setup and normal SMP execution begin.

Early boot allocations before step 3 continue using `malloc_unlocked`.
Generated allocators are never used before their singleton arena tables are
initialized.
There is no runtime cache registry or descriptor table: type size, alignment,
page capacity, and function names are compile-time properties of each
instantiation.

## 16. Integrating other vendor templates

An allocator consumer that can run on any kernel thread must not capture the
arena current when the consumer object was created. For example, binding an
LLRB tree to CPU 0's node arena at `_new` would send every later insertion
through CPU 0 even when the operation runs on CPU 3.

Instead, allocation policy is selected at each node allocation and free:

```text
LLRB operation on current CPU
        │
        ▼
generated node slab malloc/free
  call SLAB_WHICH_CPU()
  select the internal arena
        │
        ▼
local alloc, local free, or remote free
```

The slab template remains unaware of both LLRB and CPUs. The LLRB template
remains unaware of the kernel and continues to default to standard C
allocation.

### LLRB allocation hooks

The current `LLRB_MALLOC(size)` hook covers two different types: the tree
object and each node. A fixed-type slab cannot safely serve both. `llrb_impl.h`
should split the policy points:

```c
#ifndef LLRB_MALLOC
#define LLRB_MALLOC(size) malloc(size)
#endif
#ifndef LLRB_FREE
#define LLRB_FREE(ptr) free(ptr)
#endif
#ifndef LLRB_TREE_MALLOC
#define LLRB_TREE_MALLOC(size) LLRB_MALLOC(size)
#endif
#ifndef LLRB_TREE_FREE
#define LLRB_TREE_FREE(tree) LLRB_FREE(tree)
#endif
#ifndef LLRB_NODE_MALLOC
#define LLRB_NODE_MALLOC(size) LLRB_MALLOC(size)
#endif
#ifndef LLRB_NODE_FREE
#define LLRB_NODE_FREE(node) LLRB_FREE(node)
#endif
```

The implementation calls them with the actual generated type size:

```c
LLRB_T *tree = LLRB_TREE_MALLOC(sizeof(*tree));
LLRB_NODE_T *node = LLRB_NODE_MALLOC(sizeof(*node));
```

Existing `LLRB_MALLOC`/`LLRB_FREE` definitions remain compatibility defaults
for all four operations. A kernel instance normally overrides only the node
hooks; the few tree objects can stay on `malloc`, be embedded later, or receive
their own distinct slab instance. Keeping `size` in both malloc hooks makes
their default and fallback definitions direct delegations to standard
`malloc(size)`.

The generated `LLRB_NODE_T` storage definition must be available to its slab
implementation so `sizeof` and `alignof` are legal. The simplest arrangement
is to move the already-generated node structure definition from
`llrb_impl.h` to `llrb.h`; it remains namespaced by `LLRB_NAME`. It may still
be documented as allocator-private even though its layout is visible. This
allows an ordinary declaration:

```c
#define SLAB_NAME llrb_pid_process_node
#define SLAB_TYPE llrb_pid_process_node
#include <slab/slab.h>
```

and one corresponding `slab_impl.h` translation unit.

The LLRB implementation instance connects the generated slab directly:

```c
#define LLRB_NODE_MALLOC(size) \
  slab_llrb_pid_process_node_malloc(size)
#define LLRB_NODE_FREE(node) \
  slab_llrb_pid_process_node_free(node)
#include <llrb/llrb_impl.h>
```

There is no indirect callback on the tree operation and no arena pointer in
the tree. Any kernel thread may call the LLRB under the tree's existing
external synchronization. The node slab calls its configured
`SLAB_WHICH_CPU()` for that operation; if it frees a node allocated from
another CPU's slab, the stored owner-arena pointer sends that node through
`thread_free`.

In a hosted use, the node slab can select a standard `_Thread_local` index, or
the LLRB hooks can simply retain their standard `malloc/free` defaults.

This pattern applies equally to other vendor templates: expose separate typed
malloc/free hooks for each generated storage type, retain the size argument,
and connect those hooks directly to a generated slab singleton. The container
never needs to know where it is running.

## 17. Implementation sequence

1. Add standards-conforming `aligned_alloc` to the kernel's freestanding
   `stdlib`.
2. Implement the declaration and implementation templates with hosted tests
   for sizing, page layout, arena initialization, and same-owner
   allocation/free.
3. Add the atomic `thread_free` pointer without full-slab removal; stress
   remote push and owner collection with hosted C threads.
4. Add `TF_FULL`, `TF_QUEUED`, and the remote-ready notification protocol.
5. Add `TF_RETIRED` and empty-page return through standard `free`.
6. Add generated singleton initialization and the `SLAB_WHICH_CPU()` policy
   hook, then validate the kernel's pinning contract at every call context.
7. Instantiate and convert `thread` and `process`, retaining their current
   zero-allocation semantics.
8. Convert `ublock`, `ring`, capability, and IOMMU metadata one generated
   instance at a time.
9. Add debug poisoning, generated arena verification, and counters.
10. Measure remote CAS retries, page occupancy, buddy refills, retained warm
    slabs, and cache-line-alignment costs before adding further policy.

## 18. Required tests

The vendor template is tested in a hosted C program using only its default
standard-library boundary:

- Instantiate at least two different object types and verify that their
  generated names and type tags do not collide.
- Fill and drain a slab while `SLAB_WHICH_CPU()` selects one arena.
- Give each hosted thread a distinct `_Thread_local` callback index, have every
  other thread remotely free objects from one slab, and verify that each
  object is collected exactly once.
- Remotely free into a full slab while its owner concurrently attempts the
  `TF_AVAILABLE -> TF_FULL` transition.
- Race additional remote frees with `TF_QUEUED -> TF_AVAILABLE` collection
  and verify that no object is stranded.
- Make the last live objects remote frees and verify that reclamation cannot
  precede their publication or accounting.
- Repeatedly retire and reuse the same aligned allocation to exercise ABA-like
  address reuse in the remote-ready stack.
- Override `SLAB_ALLOC_PAGE` for deterministic OOM and verify a clean `NULL`
  result without corrupting the active slab.
- Verify cross-type, misaligned, duplicate, and post-retirement frees are
  diagnosed in debug builds.
- Verify `_destroy` rejects live objects and releases every page after
  quiescence.

Kernel integration tests then cover the policy callback and its execution
contract:

- Interleave owner allocation and local free with nested IRQ-disable levels.
- Exercise remote frees from ordinary interrupt context.
- Exhaust the buddy allocator through `aligned_alloc` refill and preserve the
  generated arena.
- Compare buddy page consumption before and after converting each fixed-size
  type.

## 19. Deferred extensions

- Batch a chain of remote objects in one CAS if measurements show heavy
  contention on a single slab's `thread_free`.
- Add per-type reserves for allocation in hard interrupt paths.
- Add multi-page slabs for structures too large to pack efficiently in one
  page.
- Route general small `malloc` sizes through slab caches only if a reliable,
  explicit allocation-kind scheme is introduced; typed caches do not depend
  on this.
- Add CPU-offline draining and slab ownership transfer if CPU hot-unplug
  becomes a kernel feature.
