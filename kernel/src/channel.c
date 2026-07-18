#include "channel_internal.h"
#include "capability.h"

#include <stdatomic.h>
#include <stdint.h>

#include "debug.h"
#include "iommu.h"
#include "process.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"

#include <gdosabi/thread.h>

// Core channel plumbing: rings, wakes, the data-plane syscalls, and
// teardown. Per-scheme logic lives in schemes/{shares,tree,irq}.c; shared
// internals in channel_internal.h.
//
// Locking (hierarchy in umem.h). Two planes:
//
//   Data plane — channel_block_wait and channel_block_doorbell on user
//   channels and private event blocks. Takes the caller's list lock to
//   resolve the block, then one stripe, never g_umem. The stripe serializes
//   park vs wake vs tear (the lossless compare-and-park protocol); the list
//   lock pins the block.
//
//   Control plane — ring drains, scheme replays, and teardown: all under
//   g_umem. CQ publication itself takes the ring block's stripe.

// ---------------------------------------------------------------------------
// Side classification + wakes
// ---------------------------------------------------------------------------

static bool classify_side(struct process *p, ublock *b, bool *owner_out,
                          bool *local_out) {
  *local_out = false;
  if (b->ring != nullptr) {
    if (b->owner != p) {
      return false; // ring blocks have no sharers: only the owner side
    }
    *owner_out = true;
    return true;
  }
  uint32_t nsharers = vec_share_edge_len(b->sharers);
  if (nsharers == 0) {
    if (b->owner != p) {
      return false;
    }
    *owner_out = true;
    *local_out = true;
    return true;
  }
  if (nsharers != 1) {
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
  // Dead threads never return to userspace. Leaving the detached TCB
  // blocked lets bounded process reap free it without a runqueue burst.
  if (process_is_dead(t->proc)) {
    return;
  }
  thread_deliver_wait_result(t, result);
  thread_unblock(t);
}

static void private_wait_push(ublock *b, struct thread *t) {
  asserts(t->wait_prev == nullptr && t->wait_next == nullptr,
          "channel: thread already wait-linked");
  t->wait_prev = b->private_wait_tail;
  if (b->private_wait_tail != nullptr)
    b->private_wait_tail->wait_next = t;
  else
    b->private_wait_head = t;
  b->private_wait_tail = t;
}

static struct thread *private_wait_pop(ublock *b) {
  struct thread *t = b->private_wait_head;
  if (t == nullptr)
    return nullptr;
  b->private_wait_head = t->wait_next;
  if (b->private_wait_head != nullptr)
    b->private_wait_head->wait_prev = nullptr;
  else
    b->private_wait_tail = nullptr;
  t->wait_prev = nullptr;
  t->wait_next = nullptr;
  return t;
}

static void wake_thread(struct thread *t, uint64_t result) {
  if (process_is_dead(t->proc))
    return;
  thread_deliver_wait_result(t, result);
  thread_unblock(t);
}

// Caller holds the block stripe. Returns true once the private FIFO is empty.
static bool private_wake_batch(ublock *b, uint64_t result) {
  for (uint32_t n = 0; n < BLOCK_WAKE_BATCH; n++) {
    struct thread *t = private_wait_pop(b);
    if (t == nullptr)
      return true;
    wake_thread(t, result);
  }
  return b->private_wait_head == nullptr;
}

// Post one CQE. Full against the *user-owned* consumption index, read
// once and never trusted: a cq_head run ahead of cq_count makes the
// in-flight count wrap huge and the channel look permanently full,
// starving only the liar. Callers treat a false return as "leave the
// event pending in its level state" (notified bits, dead children, IRQ
// route state); completions for consumed SQEs are dropped instead, which
// can only hit a user violating the sizing rules.
bool ring_post_locked(struct ring *ring, uint64_t type, uint64_t a,
                      uint64_t b, uint64_t status) {
  struct kring_hdr *h = hdr_of(ring);
  uint32_t consumed = atomic_load_explicit(&h->cq_head, memory_order_acquire);
  if (ring->cq_count - consumed >= ring->nslots) {
    return false;
  }
  cq_of(ring)[ring->cq_count % ring->nslots] =
      (struct kcqe){.type = type, .a = a, .b = b, .status = status};
  ring->cq_count++;
  atomic_store_explicit(&h->cq_count, ring->cq_count, memory_order_release);
  // The post is itself a wake on the ring's owner side. The waiter slot
  // and the CQ publication are serialized by the stripe we hold.
  wake_slot(side_waiter(ring->block, true), 0);
  return true;
}

bool channel_post(struct ring *ring, uint64_t type, uint64_t a, uint64_t b,
                  uint64_t status) {
  uint32_t si = umem_stripe(ring->block->base);
  umem_stripe_lock(si);
  bool posted = ring_post_locked(ring, type, a, b, status);
  umem_stripe_unlock(si);
  return posted;
}

bool channel_post_data(struct ring *ring, uint64_t type, uint64_t a,
                       uint64_t b, uint64_t status, uint32_t *index_out) {
  ublock *blk = ring->block;
  uint32_t si = umem_stripe(blk->base);
  umem_stripe_lock(si);
  bool posted = ring_post_locked(ring, type, a, b, status);
  if (posted) {
    *index_out = ring->cq_count - 1;
  }
  umem_stripe_unlock(si);
  return posted;
}

// ---------------------------------------------------------------------------
// Kernel-channel drains (control plane)
// ---------------------------------------------------------------------------

static struct ring **shares_anchor(struct process *p) { return &p->share_ch; }

static struct ring **tree_anchor(struct process *p) { return &p->tree_ch; }

static struct ring **iommu_anchor(struct process *p) { return &p->iommu_ch; }

static const struct scheme_ops g_schemes[] = {
    {.id = KSCHEME_CAP, .exec = cap_exec},
    {.id = KSCHEME_SHARES,
     .replay = shares_replay,
     .anchor = shares_anchor},
    {.id = KSCHEME_TREE, .replay = tree_replay, .anchor = tree_anchor},
    {.id = KSCHEME_IRQ,
     .exec = irq_exec,
     .replay = irq_replay,
     .destroy = irq_endpoint_destroy},
    {.id = KSCHEME_TIMER,
     .init = timer_endpoint_init,
     .exec = timer_exec,
     .replay = timer_replay,
     .destroy = timer_endpoint_destroy},
    {.id = KSCHEME_IOMMU,
     .exec = iommu_exec,
     .replay = iommu_replay,
     .destroy = iommu_endpoint_destroy,
     .anchor = iommu_anchor,
     .destroyable = iommu_endpoint_destroyable},
};

static const struct scheme_ops *scheme_lookup(int64_t id) {
  for (size_t i = 0; i < sizeof(g_schemes) / sizeof(g_schemes[0]); i++) {
    if (g_schemes[i].id == id) {
      return &g_schemes[i];
    }
  }
  return nullptr;
}

static uint64_t scheme_exec(struct thread *curr, struct ring *ring,
                            struct ksqe *sqe) {
  if (ring->ops->exec == nullptr) {
    // Pure event schemes take no commands; a submitted SQE still consumes
    // its slot and completes, carrying the error.
    return SYSERR_NOSYS;
  }
  return ring->ops->exec(curr, ring, sqe);
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
  if (ring->ops->replay != nullptr) {
    ring->ops->replay(ring);
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
  if (b == nullptr || b->backing != UBLOCK_RAM || b->ring != nullptr ||
      atomic_load(&b->thread_pins) != 0 ||
      vec_share_edge_len(b->sharers) != 0) {
    umem_proc_unlock(p);
    umem_unlock();
    return SYSERR_INVAL;
  }
  const struct scheme_ops *ops = scheme_lookup(scheme);
  if (ops == nullptr) {
    umem_proc_unlock(p);
    umem_unlock();
    return SYSERR_INVAL;
  }
  struct ring **anchor = ops->anchor != nullptr ? ops->anchor(p) : nullptr;
  if (anchor != nullptr && *anchor != nullptr) {
    umem_proc_unlock(p);
    umem_unlock();
    return SYSERR_EXIST;
  }
  if (!channel_block_private_idle(b)) {
    umem_proc_unlock(p);
    umem_unlock();
    return SYSERR_EXIST;
  }

  struct ring *ring = calloc(1, sizeof(*ring));
  asserts(ring != nullptr, "channel: ring alloc failed");
  ring->ops = ops;
  ring->block = b;
  ring->nslots = KRING_NSLOTS(b->order);
  if (ops->init != nullptr) {
    uint64_t init_status = ops->init(ring);
    if (init_status != 0) {
      free(ring);
      umem_proc_unlock(p);
      umem_unlock();
      return init_status;
    }
  }
  // The kernel is the trusted producer: it owns the header from here on.
  // The empty-private-queue check above and p's list lock prevent a local
  // waiter from entering until b->ring is published below.
  channel_block_torn(b, false);
  struct kring_hdr *h = (struct kring_hdr *)b->base;
  memset(h, 0, sizeof(*h));
  h->nslots = ring->nslots;

  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  b->ring = ring;
  umem_stripe_unlock(si);
  umem_proc_unlock(p);

  if (anchor != nullptr) {
    *anchor = ring;
  }
  // Level state that predates the channel announces itself now: share
  // edges with clear notified bits, children already dead.
  if (ops->replay != nullptr) {
    ops->replay(ring);
  }
  umem_unlock();
  return 0;
}

uint64_t channel_block_doorbell(struct thread *curr, uint64_t base) {
  struct process *p = curr->proc;
  // User-channel and private-block wakes run on list lock + one stripe.
  // Only kernel-ring doorbells (drains) go to the control plane.
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, 1);
  if (b == nullptr) {
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  bool owner, local;
  if (!classify_side(p, b, &owner, &local)) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  if (b->freeing) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_DEAD;
  }
  if (b->ring != nullptr) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return doorbell_ring(curr, base);
  }
  // A private event wakes one bounded FIFO batch. A two-party channel keeps
  // its structural one-waiter-per-side contract and wakes the peer slot.
  bool drained = true;
  if (local)
    drained = private_wake_batch(b, 0);
  else
    wake_slot(side_waiter(b, !owner), 0);
  umem_stripe_unlock(si);
  umem_proc_unlock(p);
  return drained ? 0 : SYSERR_AGAIN;
}

uint64_t channel_block_wait(struct thread *curr, uint64_t addr,
                            uint64_t expected) {
  struct process *p = curr->proc;
  if (addr % 4 != 0) {
    return SYSERR_INVAL;
  }
  umem_proc_lock(p);
  // A kill can land between the dispatcher's checkpoint and here. An
  // effectively dead process must not install a new waiter; existing
  // blocked waiters are detached as their resources are reaped.
  if (process_is_dead(p)) {
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
  bool owner, local;
  if (!classify_side(p, b, &owner, &local)) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  if (b->freeing) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_DEAD;
  }
  if (b->ring == nullptr && !local) {
    // Fail fast if the peer process is no longer live: only threads
    // already parked at its death wait for reap-time revocation. (The
    // read is racy against a concurrent kill; losing that race just
    // means parking and being woken SYSERR_DEAD by the reap-time tear.)
    struct process *peer =
        owner ? vec_share_edge_at(b->sharers, 0)->to : b->owner;
    if (process_is_dead(peer)) {
      umem_stripe_unlock(si);
      umem_proc_unlock(p);
      return SYSERR_DEAD;
    }
  }
  if (!local && *side_waiter(b, owner) != nullptr) {
    umem_stripe_unlock(si);
    umem_proc_unlock(p);
    return SYSERR_EXIST; // one parked thread per side / private block
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
  if (local)
    private_wait_push(b, curr);
  else
    *side_waiter(b, owner) = curr;
  umem_stripe_unlock(si);
  umem_proc_unlock(p);
  uthread_park_blocked();
}

// ---------------------------------------------------------------------------
// Revoke-path hooks (umem.c / process.c, under g_umem)
// ---------------------------------------------------------------------------

void channel_block_torn(ublock *b, bool destroy_endpoint) {
  // Caller holds g_umem, not the stripe. Normally the identity-breaking
  // mutation is already visible, so new parkers fail classification.
  // Scheme creation instead holds the owner's list lock until it publishes
  // b->ring. Either exclusion makes clearing the old slots final.
  uint32_t si = umem_stripe(b->base);
  struct ring *ring = nullptr;
  umem_stripe_lock(si);
  // Identity changes retain one structural waiter per side. Private queues
  // are drained only by the resumable VM_FREE path below.
  asserts(b->private_wait_head == nullptr,
          "channel: identity changed with private waiters");
  wake_slot(&b->owner_waiter, SYSERR_DEAD);
  wake_slot(&b->sharer_waiter, SYSERR_DEAD);
  if (destroy_endpoint && b->ring != nullptr) {
    ring = b->ring;
    b->ring = nullptr;
  }
  umem_stripe_unlock(si);
  if (ring != nullptr) {
    struct ring **anchor = ring->ops->anchor != nullptr
                               ? ring->ops->anchor(b->owner)
                               : nullptr;
    if (anchor != nullptr) {
      asserts(*anchor == ring, "channel: scheme anchor mismatch");
      *anchor = nullptr;
    }
    if (ring->ops->destroy != nullptr) {
      ring->ops->destroy(ring);
    }
    // b->ring was cleared under the stripe, so no new data-plane holder
    // can reach it; earlier holders finished before the clear acquired it.
    free(ring);
  }
}

bool channel_block_free_step(ublock *b) {
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  b->freeing = true;
  bool private_done = private_wake_batch(b, SYSERR_DEAD);
  // Shared and kernel endpoints have at most one structural waiter per side,
  // so including them in every free step remains a strict constant bound.
  wake_slot(&b->owner_waiter, SYSERR_DEAD);
  wake_slot(&b->sharer_waiter, SYSERR_DEAD);
  bool done = private_done && b->owner_waiter == nullptr &&
              b->sharer_waiter == nullptr;
  umem_stripe_unlock(si);
  return done;
}

bool channel_block_private_idle(ublock *b) {
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  bool idle = !b->freeing && b->private_wait_head == nullptr;
  umem_stripe_unlock(si);
  return idle;
}

bool channel_block_destroyable(ublock *b) {
  return b->ring == nullptr || b->ring->ops->destroyable == nullptr ||
         b->ring->ops->destroyable(b->ring);
}

void channel_thread_complete_locked(struct process *p, ublock *block,
                                    uint64_t event) {
  asserts(block != nullptr && block->owner == p,
          "thread completion: bad block");
  asserts(atomic_load(&block->thread_pins) != 0,
          "thread completion: unpinned block");
  uint32_t si = umem_stripe(block->base);
  umem_stripe_lock(si);
  // Registration kept the block private, writable, and identity-stable.
  *(volatile _Atomic uint32_t *)event = GDOS_THREAD_COMPLETE;
  if (block->private_wait_head != nullptr) {
    // A completion word is durable and pthread permits only one joiner, but
    // drain a bounded batch so raw users of the completion ABI cannot lose a
    // wake if they chose to wait with several threads.
    private_wake_batch(block, 0);
  }
  umem_stripe_unlock(si);
  atomic_fetch_sub(&block->thread_pins, 1);
}
