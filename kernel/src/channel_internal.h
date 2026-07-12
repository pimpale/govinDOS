#ifndef channel_internal_h_INCLUDED
#define channel_internal_h_INCLUDED

#include <stdatomic.h>

#include "channel.h"

// Internals shared between channel.c (core plumbing, syscall backends,
// teardown) and the per-scheme files in schemes/. Nothing here is for
// umem.c/process.c — they use channel.h.
//
// Locking recap (full hierarchy in umem.h):
//   - CQ publication (full-check, slot write, cq_count) requires
//     stripe(ring block). SQ drain cursor (sq_head) is g_umem-only.

// One description of a kernel-channel scheme's complete lifecycle. A null
// callback means the scheme has no work for that operation. `anchor` is
// non-null for the one-per-process schemes and returns their owner slot.
struct scheme_ops {
  int64_t id;
  uint64_t (*exec)(struct thread *curr, struct ring *ring,
                   struct ksqe *sqe);
  void (*replay)(struct ring *ring);
  void (*destroy)(struct ring *ring);
  struct ring **(*anchor)(struct process *p);
  bool (*destroyable)(struct ring *ring);
};

// Ring geometry. The block is hostile user memory: only the kernel-side
// struct ring fields are trusted; header fields are mirrors.
static inline struct kring_hdr *hdr_of(const struct ring *ring) {
  return (struct kring_hdr *)ring->block->base;
}

static inline struct ksqe *sq_of(const struct ring *ring) {
  return (struct ksqe *)(ring->block->base + KRING_HDR_SIZE);
}

static inline struct kcqe *cq_of(const struct ring *ring) {
  return (struct kcqe *)(ring->block->base + KRING_HDR_SIZE +
                         (uint64_t)ring->nslots * sizeof(struct ksqe));
}

// Wrap-safe consumption test shared by every scheme that retires an
// outstanding event CQE: has the user-owned cq_head (read once, never
// trusted beyond comparison) passed the CQE at monotonic index
// ev_index? Caller holds the lock that guards the event's bookkeeping
// (the route lock for IRQ claims).
static inline bool cqe_consumed(struct ring *ring, uint32_t ev_index) {
  uint32_t consumed =
      atomic_load_explicit(&hdr_of(ring)->cq_head, memory_order_acquire);
  return (int32_t)(consumed - ev_index) > 0;
}

static inline struct thread **side_waiter(ublock *b, bool owner) {
  return owner ? &b->owner_waiter : &b->sharer_waiter;
}

// ---------------------------------------------------------------------------
// channel.c core, used by the scheme files
// ---------------------------------------------------------------------------

// Post one CQE; caller holds stripe(ring->block->base). False if the CQ
// is full (leave the event in its level state). On success the post wakes
// the ring owner's parked thread, if any.
bool ring_post_locked(struct ring *ring, uint64_t type, uint64_t a,
                      uint64_t b, uint64_t status);

// Control-plane post: g_umem held, no stripe held. Takes the ring's stripe
// around ring_post_locked.
bool channel_post(struct ring *ring, uint64_t type, uint64_t a, uint64_t b,
                  uint64_t status);

// IRQ/data-plane post: no g_umem and no stripe held. The caller supplies
// lifetime pinning (an IRQ route lock); this takes the ring stripe.
bool channel_post_data(struct ring *ring, uint64_t type, uint64_t a,
                       uint64_t b, uint64_t status, uint32_t *index_out);

// ---------------------------------------------------------------------------
// schemes/shares.c — scheme -1
// ---------------------------------------------------------------------------

// Post KEV_SHARE for every un-notified edge pointing at p, until the CQ
// fills. g_umem held, no stripes (takes p's list lock).
void shares_replay(struct ring *ring);

// ---------------------------------------------------------------------------
// schemes/tree.c — scheme -3
// ---------------------------------------------------------------------------

// Post KEV_CHILD_DEAD for every un-notified dead child, until the CQ
// fills. g_umem held, no stripes.
void tree_replay(struct ring *ring);

// ---------------------------------------------------------------------------
// schemes/irq.c — scheme -4
// ---------------------------------------------------------------------------

uint64_t irq_exec(struct thread *curr, struct ring *ring,
                  struct ksqe *sqe);
void irq_replay(struct ring *ring);
void irq_endpoint_destroy(struct ring *ring);

// iommu.c — scheme -6
uint64_t iommu_exec(struct thread *curr, struct ring *ring,
                    struct ksqe *sqe);
void iommu_endpoint_destroy(struct ring *ring);
bool iommu_endpoint_destroyable(struct ring *ring);
void iommu_replay(struct ring *ring);

#endif // channel_internal_h_INCLUDED
