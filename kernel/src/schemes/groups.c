// Scheme -2: wait-groups. The epoll-shaped scheme: channel sides
// register into a group ring, wakes on them become (at most one
// outstanding) KEV_READY, revoked views owe a final KEV_DEAD.
//
// Lock split within a registration (the reg struct):
//   - The side SLOT holding the reg pointer: stripe(registered block),
//     like every slot. All slot writes also hold g_umem.
//   - The wake-state bits (pending/armed/ev_index/dead): stripe(group
//     block) — the same stripe that publishes the group's CQ. This is
//     what makes groups_notify_locked legal from the data plane: one
//     stripe covers the dedup bits, the CQ append, and the listener
//     wake together, no g_umem.
//   - The regs vec + nregs: g_umem only (registration surgery and
//     replay are control plane; the data plane never touches the vec).
//
// Reg lifetime without g_umem on the reader side: a reg observed in a
// slot is pinned while that slot's stripe is held — every path that
// frees a reg detaches it from its slot under that stripe first, and a
// dying group marks its regs dead under stripe(G) before detaching, so
// a data-plane waker that wins the race to stripe(G) still refuses to
// touch a ring that is about to be freed.

#include "channel_internal.h"

#include <stdatomic.h>

#include "debug.h"
#include "stdlib/stdlib.h"
#include "syscall.h"
#include "thread.h"

void groups_ring_init(struct ring *ring) { vec_reg_ptr_new(&ring->regs); }

// A wake on a registered side. Two bits make this lossless: with no
// KEV_READY outstanding, post one (or arm if the CQ is full); with one
// outstanding, arm — the unconsumed event covers this wake, and the
// arming re-fires it after the consumption ack, so a wake landing
// between a drain and its ack never vanishes. Caller holds stripe(group
// block); the forward slot from ring_post_locked is always empty here
// (group-in-group is forbidden), which is what bounds the wake chain.
void groups_notify_locked(struct reg *r) {
  if (r->pending) {
    r->armed = true;
    return;
  }
  struct reg *fwd;
  if (ring_post_locked(r->group, KEV_READY, r->cookie, 0, 0, &fwd)) {
    r->pending = true;
    r->ev_index = r->group->cq_count - 1;
  } else {
    r->armed = true;
  }
  asserts(fwd == nullptr, "groups: a group ring had a registered side");
}

void groups_forward(struct reg *r) {
  uint32_t sg = umem_stripe(r->group->block->base);
  umem_stripe_lock(sg);
  if (!r->dead) {
    groups_notify_locked(r);
  }
  umem_stripe_unlock(sg);
}

// Unlink r from its group's vec and free it. g_umem held; r must
// already be detached from its side slot (under that slot's stripe), so
// no data-plane waker can still be pinning it.
static void reg_free(struct reg *r) {
  vec_reg_ptr *v = r->group->regs;
  for (uint32_t i = 0; i < vec_reg_ptr_len(v); i++) {
    struct reg *q;
    vec_reg_ptr_get(v, i, &q);
    if (q == r) {
      vec_reg_ptr_swap_and_pop(v, i);
      free(r);
      return;
    }
  }
  asserts(false, "channel: registration missing from its group");
}

void groups_reg_died(struct reg *r) {
  struct ring *group = r->group;
  uint32_t sg = umem_stripe(group->block->base);
  struct reg *fwd;
  umem_stripe_lock(sg);
  r->dead = true;
  r->b = nullptr;
  bool posted = ring_post_locked(group, KEV_DEAD, r->cookie, 0, 0, &fwd);
  umem_stripe_unlock(sg);
  asserts(fwd == nullptr, "groups: a group ring had a registered side");
  group->nregs--;
  if (posted) {
    reg_free(r);
  }
  // else: it stays on the group, flagged, replayed on the next ack.
}

// Consumption ack + level-state replay for a group: retire KEV_READYs
// the user has consumed (their slots are below cq_head), then post
// whatever the level state owes — KEV_DEAD for dead registrations,
// re-fired KEV_READY for armed ones. g_umem held; the group's stripe is
// held across the walk (bits + CQ under one lock; the vec itself is
// g_umem state).
void groups_replay(struct ring *group) {
  uint32_t sg = umem_stripe(group->block->base);
  umem_stripe_lock(sg);
  // Backwards: posting a dead reg's event removes it from the vec.
  for (uint32_t i = vec_reg_ptr_len(group->regs); i > 0; i--) {
    struct reg *r;
    vec_reg_ptr_get(group->regs, i - 1, &r);
    if (r->pending && cqe_consumed(group, r->ev_index)) {
      r->pending = false;
    }
    struct reg *fwd;
    if (r->dead) {
      if (ring_post_locked(group, KEV_DEAD, r->cookie, 0, 0, &fwd)) {
        reg_free(r);
      }
    } else if (r->armed && !r->pending) {
      if (ring_post_locked(group, KEV_READY, r->cookie, 0, 0, &fwd)) {
        r->armed = false;
        r->pending = true;
        r->ev_index = group->cq_count - 1;
      }
    }
  }
  umem_stripe_unlock(sg);
}

static uint64_t groups_add(struct thread *curr, struct ring *group,
                           uint64_t base, uint64_t cookie) {
  struct process *p = curr->proc;
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, 1);
  umem_proc_unlock(p); // b stays pinned by g_umem (drain context)
  bool owner;
  if (b == nullptr || !classify_side(p, b, &owner)) {
    return SYSERR_INVAL;
  }
  if (b->ring != nullptr && b->ring->ops->id == KSCHEME_GROUPS) {
    return SYSERR_INVAL; // group-in-group
  }
  if (b->ring == nullptr) {
    // Registering a dead peer's channel is refused the same way parking
    // on it is.
    struct process *peer =
        owner ? vec_share_edge_at(b->sharers, 0)->to : b->owner;
    if (peer->state == PROC_DEAD) {
      return SYSERR_DEAD;
    }
  }
  if (2 * (group->nregs + 1) > group->nslots) {
    return SYSERR_NOMEM; // events could overflow the CQ
  }

  struct reg *r = calloc(1, sizeof(*r));
  asserts(r != nullptr, "channel: reg alloc failed");
  r->group = group;
  r->b = b;
  r->owner_side = owner;
  r->cookie = cookie;

  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  if (*side_waiter(b, owner) != nullptr || *side_reg(b, owner) != nullptr) {
    umem_stripe_unlock(si);
    free(r);
    return SYSERR_EXIST; // registered XOR parked
  }
  *side_reg(b, owner) = r;
  // Kernel channels get backfill for free: the kernel can see its own
  // queue, so a non-empty CQ is announced after install. (User channels
  // follow the ordering discipline instead: register before acking the
  // peer and the channel is provably cold here.) The cursor read needs
  // this block's stripe, which we hold.
  bool backfill =
      b->ring != nullptr &&
      b->ring->cq_count != atomic_load_explicit(&hdr_of(b->ring)->cq_head,
                                                memory_order_acquire);
  umem_stripe_unlock(si);

  vec_reg_ptr_push(group->regs, &r);
  group->nregs++;
  if (backfill) {
    groups_forward(r);
  }
  return 0;
}

static uint64_t groups_del(struct thread *curr, struct ring *group,
                           uint64_t base) {
  struct process *p = curr->proc;
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, 1);
  umem_proc_unlock(p);
  bool owner;
  if (b == nullptr || !classify_side(p, b, &owner)) {
    return SYSERR_INVAL;
  }
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  struct reg *r = *side_reg(b, owner);
  if (r == nullptr || r->group != group) {
    umem_stripe_unlock(si);
    return SYSERR_INVAL;
  }
  // Already-posted events stay; tolerating stale cookies is userspace's
  // problem, exactly as with epoll. The side is parkable again.
  *side_reg(b, owner) = nullptr;
  umem_stripe_unlock(si);
  group->nregs--;
  reg_free(r); // detached under the stripe above: no waker pins it now
  return 0;
}

uint64_t groups_exec(struct thread *curr, struct ring *group,
                     const struct ksqe *sqe) {
  switch (sqe->op) {
  case KGROUP_ADD:
    return groups_add(curr, group, sqe->a, sqe->b);
  case KGROUP_DEL:
    return groups_del(curr, group, sqe->a);
  default:
    return SYSERR_NOSYS;
  }
}

// The group dies with its block: detach every registration (live ones
// become plain parkable sides again; nobody is told — the listener
// itself is gone). Two phases, one stripe at a time:
//   1. under stripe(G): mark every reg dead — a data-plane waker that
//      reaches stripe(G) after this refuses to touch the dying ring;
//   2. per reg, under its block's stripe: clear the side slot — after
//      which no waker can newly observe the reg, and any that already
//      had must have finished (the clear waited on their stripe).
// Only then may the caller free the ring; the regs are freed here.
void groups_endpoint_destroy(struct ring *group) {
  uint32_t sg = umem_stripe(group->block->base);
  umem_stripe_lock(sg);
  for (uint32_t i = 0; i < vec_reg_ptr_len(group->regs); i++) {
    struct reg *r;
    vec_reg_ptr_get(group->regs, i, &r);
    r->dead = true;
  }
  umem_stripe_unlock(sg);
  while (vec_reg_ptr_len(group->regs) > 0) {
    struct reg *r;
    vec_reg_ptr_get(group->regs, 0, &r);
    vec_reg_ptr_swap_and_pop(group->regs, 0);
    if (r->b != nullptr) {
      uint32_t si = umem_stripe(r->b->base);
      umem_stripe_lock(si);
      if (*side_reg(r->b, r->owner_side) == r) {
        *side_reg(r->b, r->owner_side) = nullptr;
      }
      umem_stripe_unlock(si);
    }
    free(r);
  }
  vec_reg_ptr_delete(&group->regs);
}
