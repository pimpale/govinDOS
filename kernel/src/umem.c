#include "umem.h"

#include <stdatomic.h>

#include "allocator.h"
#include "buddy_allocator/buddy_allocator.h"
#include "channel.h"
#include "debug.h"
#include "irq.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"

// ---------------------------------------------------------------------------
// The umem lock
// ---------------------------------------------------------------------------
//
// One global lock for all ublock / registry / account state. Cross-process
// operations (share, revoke, owner-exit) would otherwise need two-process
// lock ordering; a single lock is the honest choice at this scale.
//
// It is deliberately NOT a plain spinlock: revocation paths call as_flush
// on *other* processes' address spaces while holding it (the flush must be
// inside the lock, or a concurrently dying sharer's as_free races it), and
// as_flush waits IRQs-off for remote shootdown acks. A CPU spinning here
// IRQs-off may be the very target the holder is waiting on, so the spin
// must keep servicing shootdowns — the same rule as the paging-internal
// locks.

static _Atomic bool g_umem_lock;

static inline void umem_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#else
  atomic_signal_fence(memory_order_seq_cst);
#endif
}

void umem_lock(void) {
  irq_disable();
  while (atomic_exchange_explicit(&g_umem_lock, true, memory_order_acquire)) {
    paging_service_shootdown();
    umem_relax();
  }
}

void umem_unlock(void) {
  atomic_store_explicit(&g_umem_lock, false, memory_order_release);
  irq_enable();
}

// ---------------------------------------------------------------------------
// Registry + per-uid accounts (all under the umem lock)
// ---------------------------------------------------------------------------

static vec_process_ptr *g_procs;

struct user_account {
  uint64_t uid;
  uint64_t bytes;
  uint64_t limit;
  struct user_account *next;
};

static struct user_account *g_accounts;

static struct user_account *account_for(uint64_t uid) {
  for (struct user_account *a = g_accounts; a != nullptr; a = a->next) {
    if (a->uid == uid) {
      return a;
    }
  }
  struct user_account *a = malloc(sizeof(*a));
  asserts(a != nullptr, "umem: account alloc failed");
  a->uid = uid;
  a->bytes = 0;
  a->limit = UINT64_MAX; // no quota until someone sets one
  a->next = g_accounts;
  g_accounts = a;
  return a;
}

static bool account_charge(uint64_t uid, uint64_t bytes) {
  struct user_account *a = account_for(uid);
  if (bytes > a->limit - a->bytes) {
    return false;
  }
  a->bytes += bytes;
  return true;
}

static void account_uncharge(uint64_t uid, uint64_t bytes) {
  struct user_account *a = account_for(uid);
  asserts(a->bytes >= bytes, "umem: account underflow");
  a->bytes -= bytes;
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
  umem_lock();
  bool ok = account_charge(p->uid, bytes);
  umem_unlock();
  if (!ok) {
    return nullptr;
  }

  uint64_t page_id = 0;
  spinlock_lock(&g_allocator_lock);
  buddy_status_t s = buddy_page_alloc(g_allocator, 1ull << order, &page_id);
  spinlock_unlock(&g_allocator_lock);
  if (s != BUDDY_STATUS_SUCCESS) {
    umem_lock();
    account_uncharge(p->uid, bytes);
    umem_unlock();
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
  // happen outside the lock; flush before the base escapes to the caller.
  as_flag(p->as, b->base, b->base + bytes, prot | PAGE_U);
  as_flush(p->as);

  umem_lock();
  vec_ublock_ptr_push(p->blocks, &b);
  umem_unlock();
  return base;
}

// Restore every view of `b` to pristine and flush, drop it from all
// sharer lists, and hand it back to the buddy. Caller holds the umem
// lock and has already unlinked b from its owner's list.
static void block_release_locked(ublock *b) {
  // Single teardown authority: parked peers wake SYSERR_DEAD exactly
  // where the views are torn out, and a kchan endpoint dies with its
  // block — error-on-park and fault-on-touch can never disagree.
  channel_block_torn(b, true);

  uint64_t end = b->base + ublock_bytes(b);
  as_flag(b->owner->as, b->base, end, PAGE_KERNEL_PRISTINE);
  as_flush(b->owner->as);
  for (uint32_t i = 0; i < vec_share_edge_len(b->sharers); i++) {
    share_edge e;
    vec_share_edge_get(b->sharers, i, &e);
    as_flag(e.to->as, b->base, end, PAGE_KERNEL_PRISTINE);
    as_flush(e.to->as);
    remove_block(e.to->shared_in, b);
  }
  account_uncharge(b->owner->uid, ublock_bytes(b));

  // Pristine everywhere + flushed: safe to recycle.
  spinlock_lock(&g_allocator_lock);
  buddy_status_t s = buddy_page_free(g_allocator, b->base / PAGE_SIZE);
  spinlock_unlock(&g_allocator_lock);
  asserts(s == BUDDY_STATUS_SUCCESS, "umem: buddy rejected block free");

  vec_share_edge_delete(&b->sharers);
  free(b);
}

int umem_free(struct process *p, uint64_t base, size_t len) {
  umem_lock();
  int32_t i = find_block(p->blocks, base);
  if (i < 0) {
    umem_unlock();
    return -1;
  }
  ublock *b;
  vec_ublock_ptr_get(p->blocks, (uint32_t)i, &b);
  if (len != 0) {
    uint64_t npages = ((uint64_t)len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t order = 0;
    while ((1ull << order) < npages) {
      order++;
    }
    if (order != b->order) {
      umem_unlock();
      return -1; // partial frees don't exist; blocks are the unit
    }
  }
  vec_ublock_ptr_swap_and_pop(p->blocks, (uint32_t)i);
  block_release_locked(b);
  umem_unlock();
  return 0;
}

// ---------------------------------------------------------------------------
// Per-view flags / sharing
// ---------------------------------------------------------------------------

int umem_protect_locked(struct process *p, uint64_t base, size_t len,
                        paging_flags_t prot) {
  if (base % PAGE_SIZE != 0 || len == 0 || len % PAGE_SIZE != 0) {
    return -1;
  }
  uint64_t end;
  if (__builtin_add_overflow(base, (uint64_t)len, &end)) {
    return -1;
  }
  ublock *b = umem_view_locked(p, base, len);
  if (b == nullptr) {
    return -1;
  }
  as_flag(p->as, base, end, prot == 0 ? 0 : (prot | PAGE_U));
  as_flush(p->as);
  return 0;
}

int umem_protect(struct process *p, uint64_t base, size_t len,
                 paging_flags_t prot) {
  umem_lock();
  int rc = umem_protect_locked(p, base, len, prot);
  umem_unlock();
  return rc;
}

uint64_t umem_share(struct process *p, uint64_t base, uint64_t target_pid,
                    paging_flags_t prot) {
  umem_lock();
  ublock *b = umem_owned_locked(p, base);
  struct process *target = proc_lookup(target_pid);
  if (b == nullptr || target == nullptr || target == p || b->kch != nullptr) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  if (find_edge(b->sharers, target) >= 0) {
    umem_unlock();
    return SYSERR_EXIST;
  }
  // Going from one sharer to several breaks any channel identity parked
  // waiters relied on (data-plane calls need exactly one sharer): wake
  // them out with SYSERR_DEAD rather than strand them.
  if (vec_share_edge_len(b->sharers) == 1) {
    channel_block_torn(b, false);
  }
  share_edge e = {.to = target, .notified = false};
  vec_share_edge_push(b->sharers, &e);
  vec_ublock_ptr_push(target->shared_in, &b);
  as_flag(target->as, b->base, b->base + ublock_bytes(b), prot | PAGE_U);
  as_flush(target->as);

  // Announce on the target's shares channel (or leave the edge's notified
  // bit clear for replay when the channel exists / has room).
  channel_edge_notify(
      b, vec_share_edge_at(b->sharers, vec_share_edge_len(b->sharers) - 1));
  umem_unlock();
  return 0;
}

int umem_unshare(struct process *p, uint64_t base) {
  umem_lock();
  int32_t i = find_block(p->shared_in, base);
  if (i < 0) {
    umem_unlock();
    return -1;
  }
  ublock *b;
  vec_ublock_ptr_get(p->shared_in, (uint32_t)i, &b);
  vec_ublock_ptr_swap_and_pop(p->shared_in, (uint32_t)i);
  remove_edge(b->sharers, p);
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

bool umem_reap_one_block_locked(struct process *p) {
  if (vec_ublock_ptr_len(p->blocks) == 0) {
    return false;
  }
  ublock *b;
  vec_ublock_ptr_get(p->blocks, 0, &b);
  vec_ublock_ptr_swap_and_pop(p->blocks, 0);
  // The zombie's own AS is restored too (block_release_locked): it still
  // exists until the reap's as_free step, and a killed-but-still-running
  // thread of the zombie could be touching it right now — pristinity
  // must hold before the block recycles.
  block_release_locked(b);
  return true;
}

bool umem_reap_one_view_locked(struct process *p) {
  if (vec_ublock_ptr_len(p->shared_in) == 0) {
    return false;
  }
  ublock *b;
  vec_ublock_ptr_get(p->shared_in, 0, &b);
  vec_ublock_ptr_swap_and_pop(p->shared_in, 0);
  remove_edge(b->sharers, p);
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

  // Ownership is channel identity: whoever was parked on this block was
  // waiting on a peer that no longer exists.
  channel_block_torn(b, false);

  // If the receiver already had a shared-in view, the owner view below
  // replaces it (and its edge).
  int32_t si = find_block(to->shared_in, b->base);
  if (si >= 0) {
    vec_ublock_ptr_swap_and_pop(to->shared_in, (uint32_t)si);
    remove_edge(b->sharers, to);
  }

  remove_block(from->blocks, b);
  vec_ublock_ptr_push(to->blocks, &b);
  b->owner = to;

  if (src_as_live) {
    as_flag(from->as, b->base, b->base + bytes, PAGE_KERNEL_PRISTINE);
    as_flush(from->as);
  }
  as_flag(to->as, b->base, b->base + bytes, PAGE_R | PAGE_W | PAGE_U);
  as_flush(to->as);
  return 0;
}

// ---------------------------------------------------------------------------
// Registry maintenance (process.c, under the umem lock)
// ---------------------------------------------------------------------------

struct process *umem_proc_lookup_locked(uint64_t pid) {
  return proc_lookup(pid);
}

void umem_proc_unregister_locked(struct process *p) {
  remove_proc(g_procs, p);
}
