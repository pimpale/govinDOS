#ifndef channel_h_INCLUDED
#define channel_h_INCLUDED

#include <stdint.h>

#include "umem.h"

// Shared-block channels + kernel schemes (ipc-process-design.md §1, §2).
//
// A shared ublock is a channel both sides may treat as a ring. An owned,
// unshared ublock is also a process-private event block. The kernel
// contributes exactly two data-plane verbs scoped to the block —
// SYS_BLOCK_DOORBELL (wake the peer, self-wake a private block, or run kernel
// commands) and SYS_BLOCK_WAIT (futex-shaped park on a 32-bit word) — plus
// consented establishment via SYS_VM_SHARE. Between user peers everything
// inside the block is userspace convention; when the far end is a kernel
// scheme (VM_SHARE to a negative id) the layout below is kernel ABI.
//
// There is no session object and no IPC registry: kernel-side state is
// two waiter slots per ublock, a notified bit per share edge, and one
// ring endpoint per kernel channel — the slots guarded by the block's
// stripe lock, the ring/edge state by g_umem (hierarchy in umem.h), all
// torn down by the umem revoke path (channel_block_torn).

// The kernel ring ABI — kring_hdr/ksqe/kcqe layout in gdos/kring.h, one
// gdos/kring_*.h per scheme (ids, KEV_* event types, ops) — lives in the
// shared ABI headers (the kernel↔userspace contract). This header adds
// the kernel-side endpoint state and entry points.
#include <gdosabi/kring.h>
#include <gdosabi/kring_cap.h>
#include <gdosabi/kring_irq.h>
#include <gdosabi/kring_iommu.h>
#include <gdosabi/kring_shares.h>
#include <gdosabi/kring_tree.h>

// Kernel-channel endpoint. Lives on the ublock (b->ring); owner-only.
// CQ publication (cq_count, CQ slots, the full-check) happens under
// stripe(block->base). sq_head is g_umem-only: drains are its sole touchers.
// Holding the stripe does not by itself license posting for shares/tree:
// their posts must stay atomic with level-state flips
// (notified/death_notified) that live under g_umem.
struct scheme_ops;

struct ring {
  const struct scheme_ops *ops;
  struct ublock *block;
  uint32_t nslots;
  uint32_t sq_head;  // authoritative; header copy is a mirror
  uint32_t cq_count; // authoritative; header copy is a mirror
  // Scheme -4 only: intrusive list of static route entries. The list and
  // count are g_umem-only; each route's delivery state has its own lock.
  struct irq_route *irq_claims;
  uint32_t nclaims;
};

// ---------------------------------------------------------------------------
// Syscall backends (syscall.c). `curr` is the calling user thread; both
// may park it, so the caller must have saved its frame with rax
// preloaded to 0 before calling (the RING_WAIT discipline). A non-parking
// return value overwrites the live frame's rax as usual.
// ---------------------------------------------------------------------------

// SYS_VM_SHARE with a negative target: turn the owned block at `base`
// into a kernel channel of `scheme`. Returns 0 or SYSERR_*.
uint64_t channel_scheme_create(struct process *p, uint64_t base,
                               int64_t scheme);

// SYS_BLOCK_DOORBELL: wake the far side (user channel), the owner waiter
// (private block), or drain + execute the SQ (kernel channel). Never parks.
uint64_t channel_block_doorbell(struct thread *curr, uint64_t base);

// SYS_BLOCK_WAIT: park on the 32-bit word at `addr` until a doorbell/CQE
// post/revocation, unless it already differs from `expected`. Parks via
// uthread_park_blocked (never returning) or returns immediately.
uint64_t channel_block_wait(struct thread *curr, uint64_t addr,
                            uint64_t expected);

// ---------------------------------------------------------------------------
// Hooks called by umem.c / process.c under g_umem
// ---------------------------------------------------------------------------

// A share edge b -> target was just created: post KEV_SHARE to the
// target's shares channel if it has one with a free slot (sets the
// edge's notified bit), else leave it clear for replay. Caller must NOT
// hold a stripe (the post takes the ring block's stripe).
void channel_edge_notify(ublock *b, struct share_edge *e);

// `child` just died: post KEV_CHILD_DEAD to the parent's tree channel if
// it has one with a free slot (sets child->death_notified), else leave
// the bit clear for replay.
void channel_child_dead_notify(struct process *parent, struct process *child);

// The block is being revoked or its wait identity changed (first/second
// share, ownership move): wake both waiter slots with SYSERR_DEAD and, if
// `destroy_endpoint`, free the ring. Idempotent. Caller holds g_umem and
// not the stripe. Normally mutate then tear so new parkers fail
// classification; scheme creation may instead hold the owner's list lock
// across tear + ring publication, which excludes new private waiters.
void channel_block_torn(ublock *b, bool destroy_endpoint);

// Voluntary VM_FREE must not destroy a stateful endpoint until its scheme
// resources are gone. Reap performs the scheme cleanup first.
bool channel_block_destroyable(ublock *b);

#endif // channel_h_INCLUDED
