#ifndef channel_h_INCLUDED
#define channel_h_INCLUDED

#include <stdint.h>

#include "umem.h"

// Shared-block channels + kernel schemes (ipc-process-design.md §1, §2).
//
// A shared ublock is a channel both sides may treat as a ring. Waiting
// and waking are the futex's (futex-design.md): threads park on 32-bit
// words by address, and a FUTEX_WAKE whose address resolves to a kernel
// channel is the doorbell (channel_ring_drain). Between user peers
// everything inside the block is userspace convention; when the far end
// is a kernel scheme (VM_SHARE to a negative id) the layout below is
// kernel ABI.
//
// There is no session object and no IPC registry: kernel-side state is
// a notified bit per share edge and one ring endpoint per kernel
// channel — the ring/edge state under g_umem (hierarchy in umem.h), CQ
// publication under the block's stripe, endpoints torn down by the umem
// revoke path (channel_ring_destroy).

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

#define SLAB_NAME ring
#define SLAB_TYPE struct ring
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

// ---------------------------------------------------------------------------
// Syscall backends (syscall.c / futex.c)
// ---------------------------------------------------------------------------

// SYS_VM_SHARE with a negative target: turn the owned block at `base`
// into a kernel channel of `scheme`. Returns 0 or SYSERR_*.
uint64_t channel_scheme_create(struct process *p, uint64_t base,
                               int64_t scheme);

// FUTEX_WAKE's kernel-channel case (the old doorbell): drain + execute
// the SQ in the caller's context and run the scheme's replay. `address`
// is any address within the ring block. Never parks.
uint64_t channel_ring_drain(struct thread *curr, uint64_t address);

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

// The block is being revoked: destroy its kernel-channel endpoint, if
// any (unhook the scheme anchor, run the scheme's destroy, free the
// ring). Idempotent; a no-op for plain blocks. Caller holds g_umem and
// not the stripe. Parked futex waiters are never notified — recovery is
// the waiter's deadline (futex-design.md §5).
void channel_ring_destroy(ublock *b);

// Voluntary VM_FREE must not destroy a stateful endpoint until its scheme
// resources are gone. Reap performs the scheme cleanup first.
bool channel_block_destroyable(ublock *b);

// Publish a thread's post-deschedule completion and wake one waiter on
// the completion word. Caller holds g_umem; the TCB owns one thread_pins
// reference on `block`. The reference is consumed here.
void channel_thread_complete_locked(struct process *p, ublock *block,
                                    uint64_t event);

#endif // channel_h_INCLUDED
