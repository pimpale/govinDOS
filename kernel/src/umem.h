#ifndef umem_h_INCLUDED
#define umem_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "thread.h"

// User memory: power-of-two buddy blocks ("ublocks") granted to processes.
//
// The ordered ublock indices hanging off each process are the authority on
// ownership and lifetime of user memory; g_ublocks is the secondary global
// name index, and the page trees are the authority on access
// (user_range_ok). There is no per-frame metadata.
//
// Model (docs/technical/memory-design.md):
//   - A block is owned by exactly one process (its creator) and may be
//     shared with others. Flags are per-view: each process's re-flagging
//     of a sub-range affects only its own address space.
//   - Owner death alone changes no mappings. The authorized reaper
//     enumerates and removes share edges before freeing the block; each
//     removed view is restored to the kernel mapping, so a later ring-3
//     touch faults. Sharing is cooperative.
//   - Pristinity invariant: a block returns to the buddy only after every
//     view of it (owner + sharers) has been restored to the boot mapping
//     (PAGE_KERNEL_PRISTINE) and flushed. Whatever sub-range flags the
//     process applied are erased structurally by that whole-block
//     overwrite — the kernel never tracks or replays them.
//   - Blocks are zeroed at allocation: they recycle across protection
//     domains, so handing out old contents would be an info leak.

typedef struct ublock ublock;
typedef ublock *ublock_ptr;
typedef struct process *process_ptr;

// One share of a block to one process. The edge doubles as the share
// notification queue: `notified` is set once KEV_SHARE has been posted to
// the target's shares channel, and clear edges are replayed when that
// channel's CQ frees up (channels are level-state in the edges).
typedef struct share_edge {
  ublock *block;
  struct process *to;
  struct share_edge *pending_prev;
  struct share_edge *pending_next;
  bool notified;
} share_edge;
typedef share_edge *share_edge_ptr;

#define LLRB_NAME pid_process
#define LLRB_KEY uint64_t
#define LLRB_VALUE process_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_pid_process_node
#define SLAB_TYPE llrb_pid_process_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define LLRB_NAME pid_edge
#define LLRB_KEY uint64_t
#define LLRB_VALUE share_edge
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_pid_edge_node
#define SLAB_TYPE llrb_pid_edge_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define LLRB_NAME base_edge
#define LLRB_KEY uint64_t
#define LLRB_VALUE share_edge_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_base_edge_node
#define SLAB_TYPE llrb_base_edge_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

struct ring;
struct iommu_mapping;
typedef struct llrb_domid_map llrb_domid_map;
enum ublock_backing {
  UBLOCK_RAM,
  UBLOCK_DEVICE,
};

struct ublock {
  uint64_t base; // identity VA == PA; buddy-block aligned
  uint8_t order; // RAM buddy order; zero for device blocks
  uint64_t bytes;
  enum ublock_backing backing;
  // Device blocks retain one fixed cache type and maximum rights across all
  // views. ECAM/firmware blocks set delegatable=false.
  paging_flags_t device_flags;
  paging_flags_t kernel_flags;
  bool delegatable;
  // Owner and sharers are control-plane state guarded by g_umem.
  struct process *owner;
  llrb_pid_edge *sharers; // pid -> stable inline share edge

  // There is no per-block wait state: waiting is address-keyed
  // (futex.c), and a parked thread holds no reference to its block. The
  // kernel never wakes waiters on revocation; orderly teardown is
  // userspace's choreography and disorderly teardown is the parent's
  // (futex-design.md §5).
  //
  // Non-null iff this block is a kernel channel; owned here, freed by the
  // revoke path (channel_ring_destroy). The pointer is guarded by g_umem;
  // CQ publication uses the ring-local lock.
  struct ring *ring;
  llrb_domid_map *dma_maps; // domain id -> mapping retaining this block
  // A live thread-completion registration pins a private event block's
  // identity and writable mapping until the scheduler publishes completion.
  _Atomic uint32_t thread_pins;
};

#define LLRB_NAME base_block
#define LLRB_KEY uint64_t
#define LLRB_VALUE ublock
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_base_block_node
#define SLAB_TYPE llrb_base_block_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

// Secondary global name index. Values point into the stable inline values
// owned by per-process llrb_base_block nodes.
#define LLRB_NAME base_ublock
#define LLRB_KEY uint64_t
#define LLRB_VALUE ublock_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_base_ublock_node
#define SLAB_TYPE llrb_base_ublock_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

// One-time init (lock + registry). Call before the first user process.
void umem_init(void);

// Register a freshly created user process: allocates its ublock indices and
// enters it into the pid index. The entry remains present while the
// process is a zombie and is removed only by SYS_PROC_DESTROY.
void umem_process_register(struct process *p);

// Allocate >= len bytes (rounded up to a power-of-two page count) of
// zeroed memory owned by `p`, mapped prot|PAGE_U in p's AS only. In every
// other AS the block just remains ordinary kernel memory. Returns the
// block base or nullptr (out of memory).
void *umem_alloc(struct process *p, size_t len, paging_flags_t prot);

// Return the usable byte capacity of the block at this exact base owned by
// `p`. The capacity is the page/power-of-two-rounded allocation size, not the
// original requested length. Returns 0 if base is not an owned block.
uint64_t umem_size(struct process *p, uint64_t base);

// Create a device-backed block over an existing physical range. Success is
// zero; the identity base is the block name.
uint64_t umem_map_device(struct process *p, uint64_t base, uint64_t len,
                         uint32_t flags);

// Same operation with g_umem already held, for an atomic capability
// verification + mapping transaction.
uint64_t umem_map_device_locked(struct process *p, uint64_t base, uint64_t len,
                                uint32_t flags);

// Free an exact block owned by caller or an authorized dead descendant.
// A single bounded transaction fails with SYSERR_EXIST while anything is
// still attached — sharers, DMA pins, thread pins, an undestroyable
// endpoint — rather than driving their removal itself. On success
// restores every view to pristine, flushes in one shootdown round
// (outside the control-plane lock), and returns the block to the buddy.
// Parked futex waiters are not an attachment and are not notified.
uint64_t umem_free(struct process *caller, uint64_t base);

// Re-flag [base, base+len) — page-aligned, inside a single block `p` has
// a view of (owned or shared-in) — in p's AS only. prot == 0 makes the
// sub-range inaccessible (a user-placed guard); anything else is
// sanitized to prot|PAGE_U. Per-view: sharers' mappings are unaffected.
// Takes only p's list lock (the flag + flush run under it: that is what
// makes SYS_FUTEX_WAIT's user-word load safe against a concurrent
// guard). A parent targeting its direct child must pin the target by holding
// g_umem across the lookup and call.
int umem_protect(struct process *p, uint64_t base, size_t len,
                 paging_flags_t prot);

// Map the whole block at `base` (owned by `p`) into the process with pid
// `target_pid` as prot|PAGE_U, on the spot (consent is keeping: the target
// rejects by unsharing). Posts KEV_SHARE to the target's shares channel.
// Returns 0, SYSERR_INVAL (not owner, unknown pid, self-share, kernel
// channel block) or SYSERR_EXIST (already shared to that pid).
uint64_t umem_share(struct process *p, uint64_t base, uint64_t target_pid,
                    paging_flags_t prot);

// SYS_VM_DROPSHARE: drop pid's shared-in view (pid 0 means caller).
// A non-self pid requires reaper authority over that process.
uint64_t umem_dropshare(struct process *caller, uint64_t base, uint64_t pid);

// SYS_VM_UNSHARE: the owner's per-edge revocation — remove `pid`'s view
// of the owned block at `base`. The coercion path for a peer that never
// acks; its later touch of the block is an ordinary revocation death.
// SYSERR_INVAL if base isn't p's block or pid holds no edge.
uint64_t umem_unshare(struct process *caller, uint64_t base, uint64_t pid);

uint64_t umem_enum_sharers(struct process *caller, uint64_t base,
                           uint64_t buf, uint64_t cap, uint64_t after);
uint64_t umem_enum_blocks(struct process *caller, uint64_t pid, uint64_t buf,
                          uint64_t cap, uint64_t after);
uint64_t umem_enum_views(struct process *caller, uint64_t pid, uint64_t buf,
                         uint64_t cap, uint64_t after);

// ---------------------------------------------------------------------------
// Explicit release plumbing and ownership transfer.
// ---------------------------------------------------------------------------

// Deferred tail of a block free: the flush + buddy-return that must NOT
// run under g_umem (the flush waits for cross-CPU acks — the single
// longest thing the old big lock ever covered). Filled under g_umem by
// the unlink phase, consumed by umem_release_finish after dropping it.
// Every viewing AS is pinned (as_pin) so concurrent process destruction cannot
// as_free it out from under the flush. b == nullptr means "nothing".
struct umem_release {
  ublock *b;
  llrb_base_block_node *owned_node;
  struct address_space **ases; // owner + sharers, pinned; malloc'd
  uint32_t nases;
};

// Flush all views in ONE shootdown round, unpin, return the block to
// the buddy, and free the metadata. No locks held. Idempotent on
// a zeroed struct.
void umem_release_finish(struct umem_release *rel);

// Final process-body step: free p's already-empty memory indices.
void umem_process_finish_locked(struct process *p);

// Transfer ownership of `b` from `from` to `to`: tear the old owner's view
// (skipped when src_as_live is false), map R|W into the
// new owner, and keep sharer edges.
uint64_t umem_move_locked(ublock *b, struct process *from, struct process *to,
                          bool src_as_live);

// PID index. The ordinary lookup hides effectively dead processes; the
// raw form is used by explicit resource teardown.
struct process *umem_proc_lookup_locked(uint64_t pid);
struct process *umem_proc_lookup_any_locked(uint64_t pid);
void umem_proc_unregister_locked(struct process *p);
ublock *umem_resource_block_locked(struct process *caller, uint64_t base);

// Copy a bounded enumeration result to a wholly writable user block.
// Caller holds g_umem; cap must be in 1..VM_ENUM_BATCH.
uint64_t umem_enum_copyout_locked(struct process *caller, uint64_t buf,
                                  uint64_t cap, const uint64_t *values,
                                  uint64_t count);

// Remove an un-notified edge from its target's pending list. g_umem held.
void umem_edge_pending_unlink_locked(share_edge *e);

// ---------------------------------------------------------------------------
// The umem lock hierarchy (shared with channel.c). Two control-plane levels,
// acquired strictly top-down; each is a shootdown-servicing svclock because it
// can be held across as_flush by some path:
//
//   1. g_umem (umem_lock/umem_unlock) — the control-plane lock. Every
//      ownership-graph mutation (free, share, unshare, move, destroy, kill,
//      the pid registry) and all kernel-ring internals run under it. Blocks
//      are freed only under it, so holding it pins every ublock.
//   2. p->ulock (umem_proc_lock/unlock) — sole guard of p->blocks and
//      p->views, reads included. Finding a block in an index you hold
//      pins it: a freer unlinks from every list before tearing down.
//
// Below the hierarchy sit the futex buckets (futex.c) — plain spinlocks,
// never held across a flush: SYS_FUTEX_WAIT takes list lock -> bucket
// (the interim paging discipline) and never touches g_umem; a CQ post
// takes ring cq_lock -> bucket for its wake. Bucket holders take only the
// per-CPU timer locks, g_allocator_lock, and the scheduler lock.
//   - never take a list lock or a ring CQ lock while holding a bucket;
//   - never touch a block after dropping the lock that pinned it.
// ---------------------------------------------------------------------------

void umem_lock(void);
void umem_unlock(void);

void umem_proc_lock(struct process *p);
void umem_proc_unlock(struct process *p);

// Block with this exact base owned by `p`, or nullptr. Caller holds
// p's list lock (umem_proc_lock), or g_umem plus certainty that p
// cannot allocate concurrently.
ublock *umem_owned_locked(struct process *p, uint64_t base);
// Block containing [addr, addr+len) that `p` has a view of (owned or
// shared-in), or nullptr. Same locking contract.
ublock *umem_view_locked(struct process *p, uint64_t addr, uint64_t len);

#endif // umem_h_INCLUDED
