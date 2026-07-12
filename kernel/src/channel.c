#include "channel_internal.h"

#include <stdatomic.h>
#include <stdint.h>

#include "debug.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"

// Core channel plumbing: rings, wakes, the data-plane syscalls, and
// teardown. Per-scheme logic lives in schemes/{shares,tree,groups}.c;
// shared internals in channel_internal.h.
//
// Locking (hierarchy in umem.h). Two planes:
//
//   Data plane — channel_block_wait and channel_block_doorbell on user
//   channels, INCLUDING delivery into a wait-group when the far side is
//   registered. Takes the caller's list lock to resolve the block, then
//   stripes, never g_umem. The stripe serializes park vs wake vs tear
//   (lossless wake protocol); the list lock pins the block; a reg read
//   from a slot is pinned while that slot's stripe is held.
//
//   Control plane — ring drains, scheme replays, registration surgery,
//   teardown: all under g_umem, exactly as before the split.
//
// Stripe pairing discipline: a KEV_READY delivery touches two blocks —
// the registered channel (its slot, under stripe(b)) and the group ring
// (its CQ + reg wake-state, under stripe(G)). The data plane holds both
// simultaneously, acquired in ASCENDING stripe-index order; when the
// discovered order is descending it releases, reacquires ascending, and
// revalidates the slot (wake_user_side_ranked). All data-plane pair
// holders acquiring in index order cannot cycle. Control-plane code
// never holds two stripes at once: g_umem pins regs and rings, so it
// posts in sequential single-stripe sections (channel_post, then
// groups_forward). A control-plane single-stripe holder never waits on
// a second stripe, so it cannot join a cycle either.

// ---------------------------------------------------------------------------
// Side classification + wakes
// ---------------------------------------------------------------------------

bool classify_side(struct process *p, ublock *b, bool *owner_out) {
  if (b->ring != nullptr) {
    if (b->owner != p) {
      return false; // ring blocks have no sharers: only the owner side
    }
    *owner_out = true;
    return true;
  }
  if (vec_share_edge_len(b->sharers) != 1) {
    return false;
  }
  bool is_owner = b->owner == p;
  if (!is_owner && vec_share_edge_at(b->sharers, 0)->to != p) {
    return false;
  }
  *owner_out = is_owner;
  return true;
}

// Wake the thread parked in `slot` (if any) with `result` as its syscall
// return value. Caller holds the stripe of the slot's block: that is the
// SPSC guarantee thread_unblock relies on — only one waker can extract a
// given thread from its slot. thread_unblock spins on the parker's
// in-flight context save (which needs nothing we hold — the park path
// drops all channel locks before touching scheduler state) and takes a
// scheduler lock, whose holders never wait on any umem-hierarchy lock.
static void wake_slot(struct thread **slot, uint64_t result) {
  struct thread *t = *slot;
  if (t == nullptr) {
    return;
  }
  *slot = nullptr;
  thread_deliver_wait_result(t, result);
  thread_unblock(t);
}

// Post one CQE. Full against the *user-owned* consumption index, read
// once and never trusted: a cq_head run ahead of cq_count makes the
// in-flight count wrap huge and the channel look permanently full,
// starving only the liar. Callers treat a false return as "leave the
// event pending in its level state" (notified bits, dead children,
// armed/dead registrations); completions for consumed SQEs are dropped
// instead, which can only hit a user violating the sizing rules.
bool ring_post_locked(struct ring *ring, uint64_t type, uint64_t a,
                      uint64_t b, uint64_t status, struct reg **fwd_out) {
  *fwd_out = nullptr;
  struct kring_hdr *h = hdr_of(ring);
  uint32_t consumed = atomic_load_explicit(&h->cq_head, memory_order_acquire);
  if (ring->cq_count - consumed >= ring->nslots) {
    return false;
  }
  cq_of(ring)[ring->cq_count % ring->nslots] =
      (struct kcqe){.type = type, .a = a, .b = b, .status = status};
  ring->cq_count++;
  atomic_store_explicit(&h->cq_count, ring->cq_count, memory_order_release);
  // The post is itself a wake on the ring's owner side. The slot lives
  // under the stripe we hold; a registration is handed out instead of
  // delivered (a second stripe must not nest here).
  struct reg *r = *side_reg(ring->block, true);
  if (r != nullptr) {
    *fwd_out = r;
  } else {
    wake_slot(side_waiter(ring->block, true), 0);
  }
  return true;
}

bool channel_post(struct ring *ring, uint64_t type, uint64_t a, uint64_t b,
                  uint64_t status) {
  uint32_t si = umem_stripe(ring->block->base);
  struct reg *fwd;
  umem_stripe_lock(si);
  bool posted = ring_post_locked(ring, type, a, b, status, &fwd);
  umem_stripe_unlock(si);
  if (fwd != nullptr) {
    groups_forward(fwd); // pinned by g_umem; bounded: groups never forward
  }
  return posted;
}

// Data-plane wake of one side of a user channel, following a
// registration into its group without g_umem. Entered with stripe(b)
// (== `si`) held; returns with all stripes released. The caller keeps
// holding a lock that pins b itself (its list lock, or g_umem).
//
// Pinning chain: while stripe(b) is held and r sits in the slot, r and
// r->group's ring cannot be freed (every detach path clears the slot
// under stripe(b) first, and frees happen strictly after detach). So
// r->group may be dereferenced to find the second stripe. Ascending
// acquisition; on descending discovery: release, reacquire in order,
// revalidate the slot — it may have changed or even been ABA-reused, in
// which case only the freshly-read value is trusted. r->dead (set under
// stripe(G) before a dying group detaches) guards the delivery: a dead
// reg's listener is gone, so the wake is dropped, not delivered into a
// ring that may be mid-teardown.
static void wake_user_side_ranked(ublock *b, bool side, uint32_t si) {
  for (;;) {
    struct reg *r = *side_reg(b, side);
    if (r == nullptr) {
      wake_slot(side_waiter(b, side), 0);
      umem_stripe_unlock(si);
      return;
    }
    uint32_t sg = umem_stripe(r->group->block->base);
    if (sg == si) {
      if (!r->dead) {
        groups_notify_locked(r);
      }
      umem_stripe_unlock(si);
      return;
    }
    if (sg > si) {
      umem_stripe_lock(sg);
      if (!r->dead) {
        groups_notify_locked(r);
      }
      umem_stripe_unlock(sg);
      umem_stripe_unlock(si);
      return;
    }
    // sg < si: reacquire ascending and revalidate.
    umem_stripe_unlock(si);
    umem_stripe_lock(sg);
    umem_stripe_lock(si);
    struct reg *r2 = *side_reg(b, side);
    if (r2 == r && umem_stripe(r2->group->block->base) == sg) {
      if (!r2->dead) {
        groups_notify_locked(r2);
      }
      umem_stripe_unlock(si);
      umem_stripe_unlock(sg);
      return;
    }
    // The slot changed while nothing was held: drop the stale stripe
    // and redispatch on the fresh value (still holding si).
    umem_stripe_unlock(sg);
  }
}

// ---------------------------------------------------------------------------
// Kernel-channel drains (control plane)
// ---------------------------------------------------------------------------

static uint64_t scheme_exec(struct thread *curr, struct ring *ring,
                            const struct ksqe *sqe) {
  switch (ring->scheme) {
  case KSCHEME_GROUPS:
    return groups_exec(curr, ring, sqe);
  default:
    // Pure event channels take no commands; a submitted SQE still
    // consumes its slot and completes, carrying the error.
    return SYSERR_NOSYS;
  }
}

// SQEs executed per doorbell, at most: the ring's amortization bound.
// Within it the drain runs in RING_SQ_CHUNK-sized slices, dropping and
// re-acquiring g_umem between slices — a full SQ must not turn the
// control-plane lock into a machine-wide stall. Leftovers keep their
// level state and wait for the next doorbell; the user library re-rings
// until the sq_head mirror catches up to what it published.
#define RING_SQ_BATCH 1024
#define RING_SQ_CHUNK 64

// Drain up to `budget` SQEs. g_umem held, no stripe held: the sq_head
// cursor is g_umem-only state (drains are its sole touchers), and each
// completion CQE takes the ring's stripe inside channel_post. Returns
// the number consumed.
static uint32_t ring_run_chunk(struct thread *curr, struct ring *ring,
                               uint32_t budget) {
  struct kring_hdr *h = hdr_of(ring);
  uint32_t tail = atomic_load_explicit(&h->sq_tail, memory_order_acquire);
  if (tail - ring->sq_head > ring->nslots) {
    return 0; // lying sq_tail — the doorbell completes nothing
  }
  uint32_t done = 0;
  while (ring->sq_head != tail && done < budget) {
    // Copy the SQE out before publishing consumption: after the mirror
    // store the user may legally reuse the slot. The whole block is
    // hostile; only this copy is validated.
    struct ksqe sqe = sq_of(ring)[ring->sq_head % ring->nslots];
    ring->sq_head++;
    atomic_store_explicit(&h->sq_head, ring->sq_head, memory_order_release);
    uint64_t status = scheme_exec(curr, ring, &sqe);
    channel_post(ring, sqe.op, sqe.a, sqe.b, status);
    done++;
  }
  return done;
}

// The doorbell doubles as the consumption ack: the user advanced cq_head
// before ringing, so pending level-state events can replay now.
static void ring_replay(struct ring *ring) {
  switch (ring->scheme) {
  case KSCHEME_SHARES:
    shares_replay(ring->block->owner);
    break;
  case KSCHEME_TREE:
    tree_replay(ring->block->owner);
    break;
  case KSCHEME_GROUPS:
    groups_replay(ring);
    break;
  default:
    break;
  }
}

// Kernel-channel doorbell: the control plane. The block is re-resolved
// after every chunk because it can die while g_umem is dropped; a
// mid-drain death just ends the drain.
static uint64_t doorbell_ring(struct thread *curr, uint64_t base) {
  struct process *p = curr->proc;
  uint32_t budget = RING_SQ_BATCH;
  bool first = true;
  for (;;) {
    umem_lock();
    umem_proc_lock(p);
    ublock *b = umem_view_locked(p, base, 1);
    umem_proc_unlock(p);
    if (b == nullptr || b->ring == nullptr || b->owner != p) {
      umem_unlock();
      return first ? SYSERR_INVAL : 0;
    }
    uint32_t chunk = budget < RING_SQ_CHUNK ? budget : RING_SQ_CHUNK;
    uint32_t done = ring_run_chunk(curr, b->ring, chunk);
    ring_replay(b->ring);
    umem_unlock();
    budget -= done;
    first = false;
    if (done < chunk || budget == 0) {
      return 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Syscall backends
// ---------------------------------------------------------------------------

uint64_t channel_scheme_create(struct process *p, uint64_t base,
                               int64_t scheme) {
  umem_lock();
  umem_proc_lock(p);
  ublock *b = umem_owned_locked(p, base);
  umem_proc_unlock(p);
  if (b == nullptr || b->ring != nullptr ||
      vec_share_edge_len(b->sharers) != 0) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  struct ring **anchor = nullptr; // schemes that are one-per-process
  switch (scheme) {
  case KSCHEME_SHARES:
    anchor = &p->share_ch;
    break;
  case KSCHEME_TREE:
    anchor = &p->tree_ch;
    break;
  case KSCHEME_GROUPS:
    break; // many per process
  default:
    umem_unlock();
    return SYSERR_INVAL;
  }
  if (anchor != nullptr && *anchor != nullptr) {
    umem_unlock();
    return SYSERR_EXIST;
  }

  struct ring *ring = calloc(1, sizeof(*ring));
  asserts(ring != nullptr, "channel: ring alloc failed");
  ring->scheme = scheme;
  ring->block = b;
  ring->nslots = KRING_NSLOTS(b->order);
  if (scheme == KSCHEME_GROUPS) {
    groups_ring_init(ring);
  }

  // The kernel is the trusted producer: it owns the header from here on.
  // Initialized before b->ring is published — until then the block has
  // zero sharers, so no data-plane call can classify a side of it.
  struct kring_hdr *h = (struct kring_hdr *)b->base;
  memset(h, 0, sizeof(*h));
  h->nslots = ring->nslots;

  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  b->ring = ring;
  umem_stripe_unlock(si);

  // Level state that predates the channel announces itself now: share
  // edges with clear notified bits, children already dead.
  switch (scheme) {
  case KSCHEME_SHARES:
    *anchor = ring;
    shares_replay(p);
    break;
  case KSCHEME_TREE:
    *anchor = ring;
    tree_replay(p);
    break;
  default:
    break;
  }
  umem_unlock();
  return 0;
}

uint64_t channel_block_doorbell(struct thread *curr, uint64_t base) {
  struct process *p = curr->proc;
  // Data plane: any user-channel wake — including delivery into a
  // wait-group — runs on list lock + stripes alone. Only kernel-ring
  // doorbells (drains) go to the control plane.
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, 1);
  if (b == nullptr) {
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  bool owner;
  if (!classify_side(p, b, &owner)) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  if (b->ring != nullptr) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return doorbell_ring(curr, base);
  }
  // Never reads the block. The list lock stays held across the ranked
  // wake: it is what pins b through the reacquire-and-revalidate path.
  wake_user_side_ranked(b, !owner, si);
  umem_proc_unlock(p);
  return 0;
}

uint64_t channel_block_wait(struct thread *curr, uint64_t addr,
                            uint64_t expected) {
  struct process *p = curr->proc;
  if (addr % 4 != 0) {
    return SYSERR_INVAL;
  }
  umem_proc_lock(p);
  // A kill can land between the dispatcher's death check and here; a
  // dead process must not park (nobody would cull it until reap). The
  // kill's unhook sweep takes this list lock, so this cannot race past
  // it: either PROC_DEAD is visible here, or the sweep sees our thread
  // in the slot and requeues it.
  if (p->state == PROC_DEAD) {
    umem_proc_unlock(p);
    uthread_park_exit();
  }
  ublock *b = umem_view_locked(p, addr, 4);
  // The caller's own view must be readable (it could have guarded the
  // page with vm_protect); the kernel is about to load through it.
  if (b == nullptr || !user_range_ok(p, addr, 4, false)) {
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  bool owner;
  if (!classify_side(p, b, &owner)) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  if (b->ring == nullptr) {
    // Fail fast if the peer process is no longer live: only threads
    // already parked at its death wait for reap-time revocation. (The
    // read is racy against a concurrent kill; losing that race just
    // means parking and being woken SYSERR_DEAD by the reap-time tear.)
    struct process *peer =
        owner ? vec_share_edge_at(b->sharers, 0)->to : b->owner;
    if (peer->state == PROC_DEAD) {
      umem_stripe_unlock(si);
      umem_proc_unlock(p);
      return SYSERR_DEAD;
    }
  }
  if (*side_waiter(b, owner) != nullptr || *side_reg(b, owner) != nullptr) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_EXIST; // registered XOR parked; one thread per side
  }

  // The word is untrusted: only compared, never interpreted. A lying peer
  // can only misdirect waiters who chose to rendezvous with it. Loaded
  // with the list lock still held: umem_protect flags under it, so a
  // concurrent guard (prot=0) cannot yank the mapping between the
  // user_range_ok above and this load — and revocations only ever
  // restore the kernel-readable pristine mapping.
  uint32_t word = *(volatile uint32_t *)addr;
  if (word != (uint32_t)expected) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return 0;
  }
  *side_waiter(b, owner) = curr;
  umem_stripe_unlock(si);
  umem_proc_unlock(p);
  uthread_park_blocked();
}

// ---------------------------------------------------------------------------
// Revoke-path hooks (umem.c / process.c, under g_umem)
// ---------------------------------------------------------------------------

void channel_block_torn(ublock *b, bool destroy_endpoint) {
  // Caller holds g_umem, NOT the stripe, and has already made the
  // identity-breaking mutation visible (edge pushed/removed, owner
  // swapped, lists unlinked): a thread parking after that mutation
  // fails classification, and everyone who parked before it is woken
  // right here. Detach under the stripe, then post the owed KEV_DEADs
  // in single-stripe sections (the regs are pinned by g_umem).
  uint32_t si = umem_stripe(b->base);
  struct reg *own, *shr;
  struct ring *ring = nullptr;
  umem_stripe_lock(si);
  wake_slot(&b->owner_waiter, SYSERR_DEAD);
  wake_slot(&b->sharer_waiter, SYSERR_DEAD);
  own = b->owner_reg;
  b->owner_reg = nullptr;
  shr = b->sharer_reg;
  b->sharer_reg = nullptr;
  if (destroy_endpoint && b->ring != nullptr) {
    ring = b->ring;
    b->ring = nullptr;
  }
  umem_stripe_unlock(si);
  if (own != nullptr) {
    groups_reg_died(own);
  }
  if (shr != nullptr) {
    groups_reg_died(shr);
  }
  if (ring != nullptr) {
    if (ring->scheme == KSCHEME_SHARES) {
      asserts(b->owner->share_ch == ring, "channel: share_ch mismatch");
      b->owner->share_ch = nullptr;
    } else if (ring->scheme == KSCHEME_TREE) {
      asserts(b->owner->tree_ch == ring, "channel: tree_ch mismatch");
      b->owner->tree_ch = nullptr;
    } else if (ring->scheme == KSCHEME_GROUPS) {
      groups_endpoint_destroy(ring);
    }
    // b->ring was cleared under the stripe and every registration is
    // detached: nothing can newly reach this ring. Data-plane holders
    // that reached it earlier finished before the detach acquired
    // their pinning stripes.
    free(ring);
  }
}

void channel_unhook_process_locked(struct process *p) {
  // A thread can only be parked in a waiter slot of a block its process
  // has a view of, so p's blocks + shared_in cover every park site of
  // p's threads. Unhook and requeue them; the scheduler culls them at
  // dispatch (their process is dead by the time this runs).
  umem_proc_lock(p);
  for (uint32_t i = 0; i < vec_ublock_ptr_len(p->blocks); i++) {
    ublock *b;
    vec_ublock_ptr_get(p->blocks, i, &b);
    uint32_t si = umem_stripe(b->base);
    umem_stripe_lock(si);
    if (b->owner_waiter != nullptr && b->owner_waiter->proc == p) {
      struct thread *t = b->owner_waiter;
      b->owner_waiter = nullptr;
      thread_unblock(t);
    }
    umem_stripe_unlock(si);
  }
  for (uint32_t i = 0; i < vec_ublock_ptr_len(p->shared_in); i++) {
    ublock *b;
    vec_ublock_ptr_get(p->shared_in, i, &b);
    uint32_t si = umem_stripe(b->base);
    umem_stripe_lock(si);
    if (b->sharer_waiter != nullptr && b->sharer_waiter->proc == p) {
      struct thread *t = b->sharer_waiter;
      b->sharer_waiter = nullptr;
      thread_unblock(t);
    }
    umem_stripe_unlock(si);
  }
  umem_proc_unlock(p);
}
