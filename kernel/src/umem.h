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
//   - Blocks are zeroed at allocation: they recycle across processes and
//     users, so handing out old contents would be an info leak.
//   - Every allocation is charged to the owner's uid; revocation-on-death
//     means the charge never migrates.

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

struct kchan;
struct kreg;

struct ublock {
  uint64_t base; // identity VA == PA; buddy-block aligned
  uint8_t order; // bytes = PAGE_SIZE << order
  struct process *owner;
  vec_share_edge *sharers; // processes other than owner with a view

  // Channel state (channel.c, under the umem lock). Each side holds at
  // most one of: a parked thread, or a wait-group registration —
  // registered XOR parked, the SPSC rule (SYSERR_EXIST otherwise).
  struct thread *owner_waiter;
  struct thread *sharer_waiter;
  struct kreg *owner_reg;
  struct kreg *sharer_reg;
  // Non-null iff this block is a kernel channel; owned here, freed by the
  // revoke path (channel_block_torn).
  struct kchan *kch;
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
// block base or nullptr (out of memory / over the uid's limit).
void *umem_alloc(struct process *p, size_t len, paging_flags_t prot);

// Free a block owned by `p`. base must be the exact block base and len
// must round to the block size (len == 0 skips the size check). Revokes
// every sharer's view, restores all views to pristine, flushes, and only
// then returns the block to the buddy. -1 if base isn't an owned block.
int umem_free(struct process *p, uint64_t base, size_t len);

// Re-flag [base, base+len) — page-aligned, inside a single block `p` has
// a view of (owned or shared-in) — in p's AS only. prot == 0 makes the
// sub-range inaccessible (a user-placed guard); anything else is
// sanitized to prot|PAGE_U. Per-view: sharers' mappings are unaffected.
// The _locked variant is for callers already under the umem lock (the
// parent-sets-embryo-views path, which must pin the target).
int umem_protect(struct process *p, uint64_t base, size_t len,
                 paging_flags_t prot);
int umem_protect_locked(struct process *p, uint64_t base, size_t len,
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
// tree checks; these own the block mechanics). All run under the umem
// lock (the *_locked convention).
// ---------------------------------------------------------------------------

// Revoke + free one owned block of `p` / drop one of p's shared-in
// views, waking parked peers SYSERR_DEAD. False if none left.
bool umem_reap_one_block_locked(struct process *p);
bool umem_reap_one_view_locked(struct process *p);

// Final reap step: free p's (now empty) block lists.
void umem_reap_finish_locked(struct process *p);

// Transfer ownership of `b` from `from` to `to`: re-charge, tear the
// old owner's view (skipped when src_as_live is false — a reaped-away
// AS), map R|W into the new owner, keep sharer edges. 0 or SYSERR_NOMEM
// (receiver uid over quota).
uint64_t umem_move_locked(ublock *b, struct process *from, struct process *to,
                          bool src_as_live);

// Live-pid index: lookup (nullptr if dead/unknown — zombies leave the
// index at death) and removal.
struct process *umem_proc_lookup_locked(uint64_t pid);
void umem_proc_unregister_locked(struct process *p);

// ---------------------------------------------------------------------------
// Kernel-internal (channel.c): the umem lock guards all channel state too
// — waiter slots, share-edge notified bits, kchan endpoints — so the
// revoke path and the data-plane syscalls can never disagree about block
// lifetime. The locked lookups below are only valid under the lock.
// ---------------------------------------------------------------------------

void umem_lock(void);
void umem_unlock(void);

// Block with this exact base owned by `p`, or nullptr.
ublock *umem_owned_locked(struct process *p, uint64_t base);
// Block containing [addr, addr+len) that `p` has a view of (owned or
// shared-in), or nullptr.
ublock *umem_view_locked(struct process *p, uint64_t addr, uint64_t len);

#endif // umem_h_INCLUDED
