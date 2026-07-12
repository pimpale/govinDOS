#ifndef channel_internal_h_INCLUDED
#define channel_internal_h_INCLUDED

#include <stdatomic.h>

#include "channel.h"

// Internals shared between channel.c (core plumbing, syscall backends,
// teardown) and the per-scheme files in schemes/. Nothing here is for
// umem.c/process.c — they use channel.h.
//
// Locking recap (full hierarchy in umem.h, plane split in channel.c):
//   - CQ publication (full-check, slot write, cq_count) requires
//     stripe(ring block). SQ drain cursor (sq_head) is g_umem-only.
//   - Reg wake-state (pending/armed/ev_index/dead) lives under
//     stripe(group block). The regs vec + nregs are g_umem-only.
//   - A reg pointer read from a side slot is pinned exactly while the
//     reader holds that slot's stripe (all detach paths clear the slot
//     under it before the reg or its ring can be freed), or while
//     holding g_umem (all frees take it).

// One description of a kernel-channel scheme's complete lifecycle. A null
// callback means the scheme has no work for that operation. `anchor` is
// non-null for the one-per-process schemes and returns their owner slot.
struct scheme_ops {
  int64_t id;
  uint64_t (*exec)(struct thread *curr, struct ring *ring,
                   const struct ksqe *sqe);
  void (*init)(struct ring *ring);
  void (*replay)(struct ring *ring);
  void (*destroy)(struct ring *ring);
  struct ring **(*anchor)(struct process *p);
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
// (stripe(ring block) for groups regs, the route lock for irq claims).
static inline bool cqe_consumed(struct ring *ring, uint32_t ev_index) {
  uint32_t consumed =
      atomic_load_explicit(&hdr_of(ring)->cq_head, memory_order_acquire);
  return (int32_t)(consumed - ev_index) > 0;
}

static inline struct thread **side_waiter(ublock *b, bool owner) {
  return owner ? &b->owner_waiter : &b->sharer_waiter;
}

static inline struct reg **side_reg(ublock *b, bool owner) {
  return owner ? &b->owner_reg : &b->sharer_reg;
}

// ---------------------------------------------------------------------------
// channel.c core, used by the scheme files
// ---------------------------------------------------------------------------

// Classify p's side of block b (kernel channel: owner only; user
// channel: exactly one sharer, caller a participant). Caller holds
// stripe(b) or g_umem. False on rule violation.
bool classify_side(struct process *p, ublock *b, bool *owner_out);

// Post one CQE; caller holds stripe(ring->block->base). False if the CQ
// is full (leave the event in its level state). On success the post is
// a wake on the ring's owner side: a parked owner is woken here; a
// registered owner is NOT (that would nest a second stripe) — its reg
// is handed back in *fwd_out for the caller to forward once this stripe
// is released (groups_forward, control plane only). fwd_out is always
// nullptr for a groups ring: group-in-group is forbidden, so posting
// into a group can never forward again — that is what bounds the chain.
bool ring_post_locked(struct ring *ring, uint64_t type, uint64_t a,
                      uint64_t b, uint64_t status, struct reg **fwd_out);

// Control-plane post: g_umem held, NO stripe held. Takes the ring's
// stripe around ring_post_locked and runs the forward, if any (the reg
// is pinned by g_umem).
bool channel_post(struct ring *ring, uint64_t type, uint64_t a, uint64_t b,
                  uint64_t status);

// IRQ/data-plane post: no g_umem and no stripe held. The caller supplies
// lifetime pinning (an IRQ route lock). Acquires the ring and optional group
// stripes in ranked order, so an IRQ ring registered in a wait-group wakes it
// without entering the control plane.
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
// schemes/groups.c — scheme -2
// ---------------------------------------------------------------------------

// Allocate the ring's regs vec (scheme_create).
void groups_ring_init(struct ring *ring);

// KGROUP_ADD/KGROUP_DEL. Drain context: g_umem held, no stripes.
uint64_t groups_exec(struct thread *curr, struct ring *group,
                     const struct ksqe *sqe);

// A wake for registration r. Caller holds stripe(r's group block) —
// this is the ONLY groups entry point legal without g_umem, which is
// what lets the data-plane doorbell deliver KEV_READY lock-free of the
// control plane.
void groups_notify_locked(struct reg *r);

// Same wake from the control plane: g_umem held, no stripes (takes the
// group's stripe itself).
void groups_forward(struct reg *r);

// Consumption ack + level-state replay. g_umem held, no stripes.
void groups_replay(struct ring *group);

// The registered view was revoked: owes its group one KEV_DEAD. g_umem
// held, no stripes; caller has already cleared the side slot.
void groups_reg_died(struct reg *r);

// The group's own block is dying: silently detach every registration
// (there is nobody left to tell). g_umem held, no stripes; frees the
// regs and the vec, not the ring struct.
void groups_endpoint_destroy(struct ring *group);

// ---------------------------------------------------------------------------
// schemes/irq.c — scheme -4
// ---------------------------------------------------------------------------

uint64_t irq_exec(struct thread *curr, struct ring *ring,
                  const struct ksqe *sqe);
void irq_replay(struct ring *ring);
void irq_endpoint_destroy(struct ring *ring);

#endif // channel_internal_h_INCLUDED
