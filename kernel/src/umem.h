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

#define VEC_DTYPE ublock_ptr
#include <vec/vec.h>
#undef VEC_DTYPE

#define VEC_DTYPE process_ptr
#include <vec/vec.h>
#undef VEC_DTYPE

struct ublock {
  uint64_t base; // identity VA == PA; buddy-block aligned
  uint8_t order; // bytes = PAGE_SIZE << order
  struct process *owner;
  vec_process_ptr *sharers; // processes other than owner with a view
};

// One-time init (lock + registry). Call before the first user process.
void umem_init(void);

// Register a freshly created user process: allocates its ublock lists and
// enters it into the pid registry used by umem_share. Paired with the
// unregistration inside umem_destroy_process.
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
int umem_protect(struct process *p, uint64_t base, size_t len,
                 paging_flags_t prot);

// Map the whole block at `base` (owned by `p`) into the process with pid
// `target_pid` as prot|PAGE_U. -1 if not owner, unknown pid, self-share,
// or already shared to that process.
int umem_share(struct process *p, uint64_t base, uint64_t target_pid,
               paging_flags_t prot);

// Drop `p`'s shared-in view of the block at `base` (restore pristine in
// p's AS). The owner keeps the block. -1 if p has no such view.
int umem_unshare(struct process *p, uint64_t base);

// Process-exit teardown (reaper only, after the AS drain): revokes
// sharers of every owned block and returns the blocks to the buddy,
// drops p's shared-in memberships, uncharges its uid, and unregisters it.
// p's own AS is NOT touched — it is drained and about to be as_free'd.
void umem_destroy_process(struct process *p);

#endif // umem_h_INCLUDED
