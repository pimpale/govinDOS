#include "channel_internal.h"
#include "capability.h"

#include <stdatomic.h>
#include <stdint.h>

#include "debug.h"
#include "futex.h"
#include "iommu.h"
#include "process.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"

#include <gdosabi/thread.h>

// Core channel plumbing: rings, ring drains, and endpoint teardown.
// Per-scheme logic lives in schemes/{shares,tree,irq}.c and iommu.c;
// shared internals in channel_internal.h. Waiting and waking belong to
// the futex (futex.c): the only wake a ring performs is one futex wake
// on its cq_count word per posted CQE.
//
// Locking (hierarchy in umem.h): ring drains, scheme replays, and
// teardown run under g_umem; CQ publication itself takes the ring
// block's stripe, and the post's futex wake nests a bucket inside it.

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
  // One CQE means one worker is needed: wake exactly one waiter parked
  // on the ring's cq_count word. The mirror store above happens before
  // the wake, so a woken (or already-awake) consumer always observes the
  // new count.
  futex_wake_one(ring->block->base + KRING_CQ_COUNT_OFF);
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

// SQEs executed per drain, at most: the ring's amortization bound.
// Within it the drain runs in RING_SQ_CHUNK-sized slices, dropping and
// re-acquiring g_umem between slices — a full SQ must not turn the
// control-plane lock into a machine-wide stall. Leftovers keep their
// level state and wait for the next drain; the user library re-rings
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
    return 0; // lying sq_tail — the drain completes nothing
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

// The drain doubles as the consumption ack: the user advanced cq_head
// before waking the ring, so pending level-state events can replay now.
static void ring_replay(struct ring *ring) {
  if (ring->ops->replay != nullptr) {
    ring->ops->replay(ring);
  }
}

// FUTEX_WAKE resolved to a kernel channel: the control plane. The block
// is re-resolved after every chunk because it can die while g_umem is
// dropped; a mid-drain death just ends the drain.
uint64_t channel_ring_drain(struct thread *curr, uint64_t address) {
  struct process *p = curr->proc;
  uint32_t budget = RING_SQ_BATCH;
  bool first = true;
  for (;;) {
    umem_lock();
    umem_proc_lock(p);
    ublock *b = umem_view_locked(p, address, 1);
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
// Establishment + teardown (umem.c, under g_umem)
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
  // Threads already parked on words in the block are unaffected — their
  // wait is address-keyed, not block-keyed.
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

void channel_ring_destroy(ublock *b) {
  // Caller holds g_umem, not the stripe. Clearing b->ring under the
  // stripe means no new CQ post can reach the ring; earlier posters
  // finished before the clear acquired it. Parked futex waiters are not
  // notified — their recovery is their deadline (futex-design.md §5).
  uint32_t si = umem_stripe(b->base);
  struct ring *ring = nullptr;
  umem_stripe_lock(si);
  if (b->ring != nullptr) {
    ring = b->ring;
    b->ring = nullptr;
  }
  umem_stripe_unlock(si);
  if (ring == nullptr) {
    return;
  }
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
  free(ring);
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
  // Registration kept the block private, writable, and identity-stable.
  // The publish-under-bucket makes the durable one-shot transition
  // lossless against a concurrent compare-and-park; multi-joiner fan-out
  // is userspace's chain-wake (futex-design.md §4).
  futex_publish_wake(event, GDOS_THREAD_COMPLETE);
  atomic_fetch_sub(&block->thread_pins, 1);
}
