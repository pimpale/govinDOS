#include "umem.h"

#include <stdatomic.h>

#include "allocator.h"
#include "buddy_allocator/buddy_allocator.h"
#include "channel.h"
#include "debug.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"

// ---------------------------------------------------------------------------
// The lock hierarchy (contract in umem.h)
// ---------------------------------------------------------------------------
//
// g_umem is what remains of the old single umem lock: the control plane.
// It still serializes every ownership-graph mutation — cross-process
// operations (share, revoke, owner-exit) would otherwise need
// multi-process lock ordering, and serializing them is the honest choice
// at this scale. What it no longer covers is the IPC data plane (list
// lock + stripe, channel.c) and the long tail of revocation (the
// combined flush round in umem_release_finish, which runs with no locks
// held at all).

static struct svclock g_umem;

void umem_lock(void) { svclock_lock(&g_umem); }
void umem_unlock(void) { svclock_unlock(&g_umem); }

void umem_proc_lock(struct process *p) { svclock_lock(&p->ulock); }
void umem_proc_unlock(struct process *p) { svclock_unlock(&p->ulock); }

#define UMEM_NSTRIPES 64

static struct svclock g_stripes[UMEM_NSTRIPES];

uint32_t umem_stripe(uint64_t base) {
  // Fibonacci hash of the page number; blocks are buddy-aligned, so the
  // low base bits alone would collide systematically.
  return (uint32_t)(((base >> 12) * 0x9E3779B97F4A7C15ull) >> 58);
}

void umem_stripe_lock(uint32_t idx) { svclock_lock(&g_stripes[idx]); }
void umem_stripe_unlock(uint32_t idx) { svclock_unlock(&g_stripes[idx]); }

// ---------------------------------------------------------------------------
// Registry (under g_umem) + per-uid accounts (lock-free)
// ---------------------------------------------------------------------------

static vec_process_ptr *g_procs;

// Accounts are append-only and never freed, so the list is a lock-free
// CAS-push stack and charging is a CAS loop on `bytes` — quota checks
// must not funnel every allocator on the machine through one lock.
struct user_account {
  uint64_t uid;
  _Atomic uint64_t bytes;
  uint64_t limit;
  struct user_account *_Atomic next;
};

static struct user_account *_Atomic g_accounts;

static struct user_account *account_lookup(uint64_t uid) {
  for (struct user_account *a =
           atomic_load_explicit(&g_accounts, memory_order_acquire);
       a != nullptr; a = atomic_load_explicit(&a->next, memory_order_acquire)) {
    if (a->uid == uid) {
      return a;
    }
  }
  return nullptr;
}

static struct user_account *account_for(uint64_t uid) {
  struct user_account *a = account_lookup(uid);
  if (a != nullptr) {
    return a;
  }
  struct user_account *fresh = malloc(sizeof(*fresh));
  asserts(fresh != nullptr, "umem: account alloc failed");
  fresh->uid = uid;
  atomic_store_explicit(&fresh->bytes, 0, memory_order_relaxed);
  fresh->limit = UINT64_MAX; // no quota until someone sets one
  for (;;) {
    struct user_account *head =
        atomic_load_explicit(&g_accounts, memory_order_acquire);
    atomic_store_explicit(&fresh->next, head, memory_order_relaxed);
    if (atomic_compare_exchange_weak_explicit(&g_accounts, &head, fresh,
                                              memory_order_release,
                                              memory_order_acquire)) {
      return fresh;
    }
    // Lost the race; a concurrent push may have been our uid.
    a = account_lookup(uid);
    if (a != nullptr) {
      free(fresh);
      return a;
    }
  }
}

static bool account_charge(uint64_t uid, uint64_t bytes) {
  struct user_account *a = account_for(uid);
  uint64_t cur = atomic_load_explicit(&a->bytes, memory_order_relaxed);
  for (;;) {
    if (bytes > a->limit - cur) {
      return false;
    }
    if (atomic_compare_exchange_weak_explicit(&a->bytes, &cur, cur + bytes,
                                              memory_order_relaxed,
                                              memory_order_relaxed)) {
      return true;
    }
  }
}

static void account_uncharge(uint64_t uid, uint64_t bytes) {
  struct user_account *a = account_for(uid);
  uint64_t prev =
      atomic_fetch_sub_explicit(&a->bytes, bytes, memory_order_relaxed);
  asserts(prev >= bytes, "umem: account underflow");
}

static struct process *proc_lookup(uint64_t pid) {
  for (uint32_t i = 0; i < vec_process_ptr_len(g_procs); i++) {
    struct process *p;
    vec_process_ptr_get(g_procs, i, &p);
    if (p->pid == pid) {
      return p;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Small vec helpers
// ---------------------------------------------------------------------------

static inline uint64_t ublock_bytes(const struct ublock *b) {
  return (uint64_t)PAGE_SIZE << b->order;
}

// Index of the block with this exact base, or -1.
static int32_t find_block(vec_ublock_ptr *v, uint64_t base) {
  for (uint32_t i = 0; i < vec_ublock_ptr_len(v); i++) {
    ublock *b;
    vec_ublock_ptr_get(v, i, &b);
    if (b->base == base) {
      return (int32_t)i;
    }
  }
  return -1;
}

// Block containing [addr, addr+len), or nullptr.
static ublock *find_containing(vec_ublock_ptr *v, uint64_t addr, uint64_t len) {
  for (uint32_t i = 0; i < vec_ublock_ptr_len(v); i++) {
    ublock *b;
    vec_ublock_ptr_get(v, i, &b);
    if (addr >= b->base && addr + len <= b->base + ublock_bytes(b)) {
      return b;
    }
  }
  return nullptr;
}

static void remove_block(vec_ublock_ptr *v, const ublock *b) {
  int32_t i = find_block(v, b->base);
  asserts(i >= 0, "umem: block missing from list");
  vec_ublock_ptr_swap_and_pop(v, (uint32_t)i);
}

static void remove_proc(vec_process_ptr *v, const struct process *p) {
  for (uint32_t i = 0; i < vec_process_ptr_len(v); i++) {
    struct process *q;
    vec_process_ptr_get(v, i, &q);
    if (q == p) {
      vec_process_ptr_swap_and_pop(v, i);
      return;
    }
  }
  asserts(false, "umem: process missing from list");
}

// Index of the edge sharing to `p`, or -1.
static int32_t find_edge(vec_share_edge *v, const struct process *p) {
  for (uint32_t i = 0; i < vec_share_edge_len(v); i++) {
    share_edge e;
    vec_share_edge_get(v, i, &e);
    if (e.to == p) {
      return (int32_t)i;
    }
  }
  return -1;
}

static void remove_edge(vec_share_edge *v, const struct process *p) {
  int32_t i = find_edge(v, p);
  asserts(i >= 0, "umem: share edge missing");
  vec_share_edge_swap_and_pop(v, (uint32_t)i);
}

ublock *umem_owned_locked(struct process *p, uint64_t base) {
  int32_t i = find_block(p->blocks, base);
  if (i < 0) {
    return nullptr;
  }
  ublock *b;
  vec_ublock_ptr_get(p->blocks, (uint32_t)i, &b);
  return b;
}

ublock *umem_view_locked(struct process *p, uint64_t addr, uint64_t len) {
  ublock *b = find_containing(p->blocks, addr, len);
  if (b == nullptr) {
    b = find_containing(p->shared_in, addr, len);
  }
  return b;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void umem_init(void) {
  asserts(g_procs == nullptr, "umem_init: called twice");
  vec_process_ptr_new(&g_procs);
}

void umem_process_register(struct process *p) {
  asserts(g_procs != nullptr, "umem: not initialized");
  vec_ublock_ptr_new(&p->blocks);
  vec_ublock_ptr_new(&p->shared_in);
  umem_lock();
  vec_process_ptr_push(g_procs, &p);
  umem_unlock();
}

// ---------------------------------------------------------------------------
// Allocation / free
// ---------------------------------------------------------------------------

void *umem_alloc(struct process *p, size_t len, paging_flags_t prot) {
  if (len == 0) {
    return nullptr;
  }
  uint64_t npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
  uint8_t order = 0;
  while ((1ull << order) < npages) {
    order++;
  }
  uint64_t bytes = (uint64_t)PAGE_SIZE << order;

  // Reserve the charge first; roll back if the buddy can't deliver.
  if (!account_charge(p->uid, bytes)) {
    return nullptr;
  }

  uint64_t page_id = 0;
  spinlock_lock(&g_allocator_lock);
  buddy_status_t s = buddy_page_alloc(g_allocator, 1ull << order, &page_id);
  spinlock_unlock(&g_allocator_lock);
  if (s != BUDDY_STATUS_SUCCESS) {
    account_uncharge(p->uid, bytes);
    return nullptr;
  }
  uint8_t *base = (uint8_t *)(page_id * PAGE_SIZE);

  // Mandatory: blocks recycle across processes and users.
  memset(base, 0, bytes);

  ublock *b = calloc(1, sizeof(*b));
  asserts(b != nullptr, "umem: ublock alloc failed");
  b->base = (uint64_t)base;
  b->order = order;
  b->owner = p;
  vec_share_edge_new(&b->sharers);

  // The block is invisible to free/protect until pushed, so the flag can
  // happen outside any lock; flush before the base escapes to the caller.
  as_flag(p->as, b->base, b->base + bytes, prot | PAGE_U);
  as_flush(p->as);

  // The whole allocation runs without g_umem — the buddy has its own
  // lock, the charge is atomic, and the push is list-local.
  umem_proc_lock(p);
  vec_ublock_ptr_push(p->blocks, &b);
  umem_proc_unlock(p);
  return base;
}

// Phase one of freeing a block: everything that needs g_umem. Caller
// holds g_umem and has already unlinked b from its owner's list. Unlinks
// every sharer view, wakes/tears the channel state, restores every view
// to pristine (dirty-marking only), and pins each viewing AS into `rel`.
// After this returns, nothing can reach b — the expensive tail (the
// flush round and the buddy return) runs in umem_release_finish with no
// locks held.
static void block_release_prepare(ublock *b, struct umem_release *rel) {
  uint64_t end = b->base + ublock_bytes(b);
  uint32_t nsharers = vec_share_edge_len(b->sharers);

  // Unlink every sharer's view first: after this no data-plane lookup
  // can find b, and whoever already found it is drained out by the
  // stripe acquisition below (lookups hold the stripe before dropping
  // their list lock).
  for (uint32_t i = 0; i < nsharers; i++) {
    share_edge e;
    vec_share_edge_get(b->sharers, i, &e);
    umem_proc_lock(e.to);
    remove_block(e.to->shared_in, b);
    umem_proc_unlock(e.to);
  }

  // Single teardown authority: parked peers wake SYSERR_DEAD exactly
  // where the views are torn out, and a ring endpoint dies with its
  // block — error-on-park and fault-on-touch can never disagree. Torn
  // takes the stripe itself; its acquisition is the barrier that drains
  // out any data-plane holder that resolved b before the unlinks above.
  channel_block_torn(b, true);

  rel->b = b;
  rel->uid = b->owner->uid;
  rel->nases = 1 + nsharers;
  rel->ases = malloc(rel->nases * sizeof(*rel->ases));
  asserts(rel->ases != nullptr, "umem: release ctx alloc failed");

  as_flag(b->owner->as, b->base, end, PAGE_KERNEL_PRISTINE);
  as_pin(b->owner->as);
  rel->ases[0] = b->owner->as;
  for (uint32_t i = 0; i < nsharers; i++) {
    share_edge e;
    vec_share_edge_get(b->sharers, i, &e);
    as_flag(e.to->as, b->base, end, PAGE_KERNEL_PRISTINE);
    as_pin(e.to->as);
    rel->ases[1 + i] = e.to->as;
  }
}

void umem_release_finish(struct umem_release *rel) {
  if (rel->b == nullptr) {
    return;
  }
  ublock *b = rel->b;

  // Pristine everywhere + one combined shootdown round: safe to recycle.
  // The pins keep a concurrently-reaped sharer's AS alive under the
  // flush; the reaper treats pins != 0 as "still draining".
  as_flush_multi(rel->ases, rel->nases);
  for (uint32_t i = 0; i < rel->nases; i++) {
    as_unpin(rel->ases[i]);
  }
  free(rel->ases);

  spinlock_lock(&g_allocator_lock);
  buddy_status_t s = buddy_page_free(g_allocator, b->base / PAGE_SIZE);
  spinlock_unlock(&g_allocator_lock);
  asserts(s == BUDDY_STATUS_SUCCESS, "umem: buddy rejected block free");

  account_uncharge(rel->uid, ublock_bytes(b));
  vec_share_edge_delete(&b->sharers);
  free(b);
  rel->b = nullptr;
}

int umem_free(struct process *p, uint64_t base) {
  struct umem_release rel = {0};
  umem_lock();
  umem_proc_lock(p);
  int32_t i = find_block(p->blocks, base);
  if (i < 0) {
    umem_proc_unlock(p);
    umem_unlock();
    return -1;
  }
  ublock *b;
  vec_ublock_ptr_get(p->blocks, (uint32_t)i, &b);
  vec_ublock_ptr_swap_and_pop(p->blocks, (uint32_t)i);
  umem_proc_unlock(p);
  block_release_prepare(b, &rel);
  umem_unlock();
  umem_release_finish(&rel);
  return 0;
}

// ---------------------------------------------------------------------------
// Per-view flags / sharing
// ---------------------------------------------------------------------------

int umem_protect(struct process *p, uint64_t base, size_t len,
                 paging_flags_t prot) {
  if (base % PAGE_SIZE != 0 || len == 0 || len % PAGE_SIZE != 0) {
    return -1;
  }
  uint64_t end;
  if (__builtin_add_overflow(base, (uint64_t)len, &end)) {
    return -1;
  }
  // Flag + flush under the list lock: SYS_BLOCK_WAIT loads its user word
  // under the same lock, so a guard (prot == 0) can never yank a mapping
  // between wait's user_range_ok and its load.
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, len);
  if (b == nullptr) {
    umem_proc_unlock(p);
    return -1;
  }
  as_flag(p->as, base, end, prot == 0 ? 0 : (prot | PAGE_U));
  as_flush(p->as);
  umem_proc_unlock(p);
  return 0;
}

uint64_t umem_share(struct process *p, uint64_t base, uint64_t target_pid,
                    paging_flags_t prot) {
  umem_lock();
  umem_proc_lock(p);
  ublock *b = umem_owned_locked(p, base);
  umem_proc_unlock(p); // b stays pinned by g_umem
  struct process *target = proc_lookup(target_pid);
  if (b == nullptr || target == nullptr || target == p || b->ring != nullptr) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  if (find_edge(b->sharers, target) >= 0) {
    umem_unlock();
    return SYSERR_EXIST;
  }
  uint32_t nsharers = vec_share_edge_len(b->sharers);
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  share_edge e = {.to = target, .notified = false};
  vec_share_edge_push(b->sharers, &e);
  umem_stripe_unlock(si);
  // Going from one sharer to several breaks any channel identity parked
  // waiters relied on (data-plane calls need exactly one sharer): wake
  // them out with SYSERR_DEAD rather than strand them. Mutate first,
  // then tear (torn's contract): with the second edge visible nothing
  // can re-park, and whoever parked before it is woken here.
  if (nsharers == 1) {
    channel_block_torn(b, false);
  }

  umem_proc_lock(target);
  vec_ublock_ptr_push(target->shared_in, &b);
  umem_proc_unlock(target);
  as_flag(target->as, b->base, b->base + ublock_bytes(b), prot | PAGE_U);
  as_flush(target->as);

  // Announce on the target's shares channel (or leave the edge's notified
  // bit clear for replay when the channel exists / has room). Posts take
  // the ring block's stripe, so ours must be released by now.
  channel_edge_notify(
      b, vec_share_edge_at(b->sharers, vec_share_edge_len(b->sharers) - 1));
  umem_unlock();
  return 0;
}

int umem_unshare(struct process *p, uint64_t base) {
  umem_lock();
  umem_proc_lock(p);
  int32_t i = find_block(p->shared_in, base);
  if (i < 0) {
    umem_proc_unlock(p);
    umem_unlock();
    return -1;
  }
  ublock *b;
  vec_ublock_ptr_get(p->shared_in, (uint32_t)i, &b);
  vec_ublock_ptr_swap_and_pop(p->shared_in, (uint32_t)i);
  umem_proc_unlock(p);
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  remove_edge(b->sharers, p);
  umem_stripe_unlock(si);
  // The channel (if the peers treated it as one) is gone: wake parked
  // peers with SYSERR_DEAD. The block itself lives on with its owner.
  channel_block_torn(b, false);
  as_flag(p->as, b->base, b->base + ublock_bytes(b), PAGE_KERNEL_PRISTINE);
  as_flush(p->as);
  umem_unlock();
  return 0;
}

// ---------------------------------------------------------------------------
// Reap-step primitives (process.c drives these, one bounded step per
// SYS_PROC_REAP call — destruction mirrors construction)
// ---------------------------------------------------------------------------

bool umem_reap_one_block_locked(struct process *p, struct umem_release *rel) {
  umem_proc_lock(p);
  if (vec_ublock_ptr_len(p->blocks) == 0) {
    umem_proc_unlock(p);
    return false;
  }
  ublock *b;
  vec_ublock_ptr_get(p->blocks, 0, &b);
  vec_ublock_ptr_swap_and_pop(p->blocks, 0);
  umem_proc_unlock(p);
  // The zombie's own AS is restored too (block_release_prepare): it
  // still exists until the reap's as_free step, and a killed-but-still-
  // running thread of the zombie could be touching it right now —
  // pristinity must hold before the block recycles (the caller runs
  // umem_release_finish before returning to userspace).
  block_release_prepare(b, rel);
  return true;
}

bool umem_reap_one_view_locked(struct process *p) {
  umem_proc_lock(p);
  if (vec_ublock_ptr_len(p->shared_in) == 0) {
    umem_proc_unlock(p);
    return false;
  }
  ublock *b;
  vec_ublock_ptr_get(p->shared_in, 0, &b);
  vec_ublock_ptr_swap_and_pop(p->shared_in, 0);
  umem_proc_unlock(p);
  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  remove_edge(b->sharers, p);
  umem_stripe_unlock(si);
  // The owner's side of the channel wakes SYSERR_DEAD — it was parked
  // for a doorbell that will never come.
  channel_block_torn(b, false);
  as_flag(p->as, b->base, b->base + ublock_bytes(b), PAGE_KERNEL_PRISTINE);
  as_flush(p->as);
  return true;
}

void umem_reap_finish_locked(struct process *p) {
  asserts(vec_ublock_ptr_len(p->blocks) == 0 &&
              vec_ublock_ptr_len(p->shared_in) == 0,
          "umem: reap finish with resources left");
  vec_ublock_ptr_delete(&p->blocks);
  vec_ublock_ptr_delete(&p->shared_in);
}

// ---------------------------------------------------------------------------
// Ownership transfer (SYS_VM_MOVE; tree checks live in process.c)
// ---------------------------------------------------------------------------

uint64_t umem_move_locked(ublock *b, struct process *from, struct process *to,
                          bool src_as_live) {
  uint64_t bytes = ublock_bytes(b);
  if (from->uid != to->uid) {
    // Charge follows the owner: reserve the new charge before releasing
    // the old so a failure leaves everything untouched.
    if (!account_charge(to->uid, bytes)) {
      return SYSERR_NOMEM;
    }
    account_uncharge(from->uid, bytes);
  }

  // List surgery before the stripe (never the other way around). In
  // both legal moves exactly one endpoint can have live threads — down
  // is into an own embryo, up is out of an own zombie — and that live
  // endpoint is `from`, which loses sight of the block here, or `to`,
  // which only gains sight at the final push: the swap is atomic to
  // every observer that can actually run.
  umem_proc_lock(from);
  remove_block(from->blocks, b);
  umem_proc_unlock(from);
  umem_proc_lock(to);
  int32_t si_view = find_block(to->shared_in, b->base);
  if (si_view >= 0) {
    // The receiver already had a shared-in view; the owner view below
    // replaces it (and its edge).
    vec_ublock_ptr_swap_and_pop(to->shared_in, (uint32_t)si_view);
  }
  umem_proc_unlock(to);

  uint32_t si = umem_stripe(b->base);
  umem_stripe_lock(si);
  if (si_view >= 0) {
    remove_edge(b->sharers, to);
  }
  b->owner = to;
  umem_stripe_unlock(si);
  // Ownership is channel identity: whoever was parked on this block was
  // waiting on a peer that no longer exists. Mutate first, then tear.
  channel_block_torn(b, false);

  if (src_as_live) {
    as_flag(from->as, b->base, b->base + bytes, PAGE_KERNEL_PRISTINE);
  }
  as_flag(to->as, b->base, b->base + bytes, PAGE_R | PAGE_W | PAGE_U);
  struct address_space *ases[2] = {to->as, from->as};
  as_flush_multi(ases, src_as_live ? 2 : 1);

  umem_proc_lock(to);
  vec_ublock_ptr_push(to->blocks, &b);
  umem_proc_unlock(to);
  return 0;
}

// ---------------------------------------------------------------------------
// Registry maintenance (process.c, under g_umem)
// ---------------------------------------------------------------------------

struct process *umem_proc_lookup_locked(uint64_t pid) {
  return proc_lookup(pid);
}

void umem_proc_unregister_locked(struct process *p) {
  remove_proc(g_procs, p);
}
