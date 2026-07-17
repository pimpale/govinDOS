#ifndef umem_h_INCLUDED
#define umem_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "thread.h"

// User memory: power-of-two buddy blocks ("ublocks") granted to processes.
//
// The ublock lists hanging off each process are the sole authority on
// ownership and lifetime of user memory; the page trees are the authority
// on access (user_range_ok). There is no per-frame metadata.
//
// Model (docs/technical/memory-design.md):
//   - A block is owned by exactly one process (its creator) and may be
//     shared with others. Flags are per-view: each process's re-flagging
//     of a sub-range affects only its own address space.
//   - Owner death or explicit free REVOKES every sharer's view: their
//     next access takes a page fault. Sharing is cooperative; sharers
//     must expect the owner to disappear.
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
  struct process *to;
  bool notified;
} share_edge;

#define VEC_DTYPE ublock_ptr
#include <vec/vec.h>
#undef VEC_DTYPE

#define VEC_DTYPE process_ptr
#include <vec/vec.h>
#undef VEC_DTYPE

#define VEC_DTYPE share_edge
#include <vec/vec.h>
#undef VEC_DTYPE

#define LLRB_NAME pid_process
#define LLRB_KEY uint64_t
#define LLRB_VALUE process_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

struct ring;
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
  // owner and sharers are written under g_umem + stripe(base) together,
  // so holding either makes them safe to read.
  struct process *owner;
  vec_share_edge *sharers; // processes other than owner with a view

  // Wait state (channel.c, under stripe(base) — the per-block lock stripe,
  // level 3 of the hierarchy below). Each side holds at most one parked
  // thread. An owned, unshared block uses owner_waiter as a process-private
  // event; a two-party user channel uses the two slots as its endpoint waits.
  struct thread *owner_waiter;
  struct thread *sharer_waiter;
  // Non-null iff this block is a kernel channel; owned here, freed by the
  // revoke path (channel_block_torn). Written under g_umem + stripe(base)
  // like owner/sharers; ring *internals* are control-plane state (g_umem).
  struct ring *ring;
  uint32_t dma_pins; // g_umem: IOMMU leaves retaining this allocation
  // A live thread-completion registration pins a private event block's
  // identity and writable mapping until the scheduler publishes completion.
  _Atomic uint32_t thread_pins;
};

// One-time init (lock + registry). Call before the first user process.
void umem_init(void);

// Register a freshly created user process: allocates its ublock lists and
// enters it into the live-pid index. Paired with the unregistration at
// death (umem_proc_unregister_locked, below).
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

// Free a block owned by `p`. base must be the exact block base — blocks
// are the unit, so the base alone is unambiguous. Revokes every
// sharer's view, restores all views to pristine, flushes every view in
// one shootdown round (outside the control-plane lock), and only then
// returns the block to the buddy. -1 if base isn't an owned block.
int umem_free(struct process *p, uint64_t base);

// Re-flag [base, base+len) — page-aligned, inside a single block `p` has
// a view of (owned or shared-in) — in p's AS only. prot == 0 makes the
// sub-range inaccessible (a user-placed guard); anything else is
// sanitized to prot|PAGE_U. Per-view: sharers' mappings are unaffected.
// Takes only p's list lock (the flag + flush run under it: that is what
// makes SYS_BLOCK_WAIT's user-word load safe against a concurrent
// guard). Callers targeting another process (the parent-sets-embryo-
// views path) must pin the target — hold g_umem across the lookup+call.
int umem_protect(struct process *p, uint64_t base, size_t len,
                 paging_flags_t prot);

// Map the whole block at `base` (owned by `p`) into the process with pid
// `target_pid` as prot|PAGE_U, on the spot (consent is keeping: the target
// rejects by unsharing). Posts KEV_SHARE to the target's shares channel.
// Returns 0, SYSERR_INVAL (not owner, unknown pid, self-share, kernel
// channel block) or SYSERR_EXIST (already shared to that pid).
uint64_t umem_share(struct process *p, uint64_t base, uint64_t target_pid,
                    paging_flags_t prot);

// Drop `p`'s shared-in view of the block at `base` (restore pristine in
// p's AS). The owner keeps the block. -1 if p has no such view.
int umem_unshare(struct process *p, uint64_t base);

// ---------------------------------------------------------------------------
// Reap-step primitives and ownership transfer (process.c, which owns the
// tree checks; these own the block mechanics). All run under g_umem (the
// *_locked convention).
// ---------------------------------------------------------------------------

// Deferred tail of a block free: the flush + buddy-return that must NOT
// run under g_umem (the flush waits for cross-CPU acks — the single
// longest thing the old big lock ever covered). Filled under g_umem by
// the unlink phase, consumed by umem_release_finish after dropping it.
// Every viewing AS is pinned (as_pin) so a concurrent reap cannot
// as_free it out from under the flush. b == nullptr means "nothing".
struct umem_release {
  ublock *b;
  struct address_space **ases; // owner + sharers, pinned; malloc'd
  uint32_t nases;
};

// Flush all views in ONE shootdown round, unpin, return the block to
// the buddy, and free the metadata. No locks held. Idempotent on
// a zeroed struct.
void umem_release_finish(struct umem_release *rel);

// Revoke + free one owned block of `p` / drop one of p's shared-in
// views, waking parked peers SYSERR_DEAD. False if none left. The block
// variant only unlinks under g_umem: the caller must run
// umem_release_finish(rel) after dropping the lock.
bool umem_reap_one_block_locked(struct process *p, struct umem_release *rel);
bool umem_reap_one_view_locked(struct process *p);

// Final reap step: free p's (now empty) block lists.
void umem_reap_finish_locked(struct process *p);

// Transfer ownership of `b` from `from` to `to`: tear the old owner's view
// (skipped when src_as_live is false — a reaped-away AS), map R|W into the
// new owner, and keep sharer edges.
uint64_t umem_move_locked(ublock *b, struct process *from, struct process *to,
                          bool src_as_live);

// Live-pid index: lookup returns nullptr for unknown or effectively dead
// processes. Effective-dead descendants remain stored but hidden until
// bounded reap materializes them and removes their entry.
struct process *umem_proc_lookup_locked(uint64_t pid);
void umem_proc_unregister_locked(struct process *p);

// ---------------------------------------------------------------------------
// The umem lock hierarchy (shared with channel.c). Three levels, acquired
// strictly top-down; each is a shootdown-servicing svclock because each
// can be held across as_flush by some path:
//
//   1. g_umem (umem_lock/umem_unlock) — the control-plane lock. Every
//      ownership-graph mutation (free, share, unshare, move, reap, kill,
//      the pid registry) and all kernel-ring internals run under it. Blocks
//      are freed only under it, so holding it pins every ublock.
//   2. p->ulock (umem_proc_lock/unlock) — sole guard of p->blocks and
//      p->shared_in, reads included. Finding a block in a list you hold
//      pins it: a freer unlinks from every list before tearing down.
//   3. stripes (umem_stripe_*) — static lock array indexed by hash(block
//      base). Guard the block's waiter slots and serialize park vs wake vs
//      tear; the lock's storage outlives any block, which is what makes
//      lock-then-look safe without the global lock.
//
// The data plane (SYS_BLOCK_WAIT / SYS_BLOCK_DOORBELL on user channels and
// private event blocks) takes list lock -> stripe and never touches g_umem:
// unrelated blocks never contend.
// Its soundness rules:
//   - take the stripe BEFORE dropping the list lock that made the block
//     reachable, and never touch the block after dropping the stripe;
//   - never take a list lock while holding a stripe;
//   - channel data-plane paths hold only the block's one stripe.
// A future cross-block operation must pin both blocks and acquire their
// stripes in ascending index order; no current channel path needs a pair.
// ---------------------------------------------------------------------------

void umem_lock(void);
void umem_unlock(void);

void umem_proc_lock(struct process *p);
void umem_proc_unlock(struct process *p);

uint32_t umem_stripe(uint64_t base);
void umem_stripe_lock(uint32_t idx);
void umem_stripe_unlock(uint32_t idx);

// Block with this exact base owned by `p`, or nullptr. Caller holds
// p's list lock (umem_proc_lock), or g_umem plus certainty that p
// cannot allocate concurrently.
ublock *umem_owned_locked(struct process *p, uint64_t base);
// Block containing [addr, addr+len) that `p` has a view of (owned or
// shared-in), or nullptr. Same locking contract.
ublock *umem_view_locked(struct process *p, uint64_t addr, uint64_t len);

#endif // umem_h_INCLUDED
