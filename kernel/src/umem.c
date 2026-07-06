#include "umem.h"

#include <stdatomic.h>

#include "allocator.h"
#include "buddy_allocator/buddy_allocator.h"
#include "debug.h"
#include "irq.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"

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

static void umem_lock(void) {
  irq_disable();
  while (atomic_exchange_explicit(&g_umem_lock, true, memory_order_acquire)) {
    paging_service_shootdown();
    umem_relax();
  }
}

static void umem_unlock(void) {
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

  ublock *b = malloc(sizeof(*b));
  asserts(b != nullptr, "umem: ublock alloc failed");
  b->base = (uint64_t)base;
  b->order = order;
  b->owner = p;
  vec_process_ptr_new(&b->sharers);

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
// lock and has already unlinked b from its owner's list. When
// `skip_owner_as` (owner is exiting), the owner's AS is drained and
// about to be freed — flagging it would be wasted work.
static void block_release_locked(ublock *b, bool skip_owner_as) {
  uint64_t end = b->base + ublock_bytes(b);
  if (!skip_owner_as) {
    as_flag(b->owner->as, b->base, end, PAGE_KERNEL_PRISTINE);
    as_flush(b->owner->as);
  }
  for (uint32_t i = 0; i < vec_process_ptr_len(b->sharers); i++) {
    struct process *s;
    vec_process_ptr_get(b->sharers, i, &s);
    as_flag(s->as, b->base, end, PAGE_KERNEL_PRISTINE);
    as_flush(s->as);
    remove_block(s->shared_in, b);
  }
  account_uncharge(b->owner->uid, ublock_bytes(b));

  // Pristine everywhere + flushed: safe to recycle.
  spinlock_lock(&g_allocator_lock);
  buddy_status_t s = buddy_page_free(g_allocator, b->base / PAGE_SIZE);
  spinlock_unlock(&g_allocator_lock);
  asserts(s == BUDDY_STATUS_SUCCESS, "umem: buddy rejected block free");

  vec_process_ptr_delete(&b->sharers);
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
  block_release_locked(b, false);
  umem_unlock();
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
  umem_lock();
  ublock *b = find_containing(p->blocks, base, len);
  if (b == nullptr) {
    b = find_containing(p->shared_in, base, len);
  }
  if (b == nullptr) {
    umem_unlock();
    return -1;
  }
  as_flag(p->as, base, end, prot == 0 ? 0 : (prot | PAGE_U));
  as_flush(p->as);
  umem_unlock();
  return 0;
}

int umem_share(struct process *p, uint64_t base, uint64_t target_pid,
               paging_flags_t prot) {
  umem_lock();
  int32_t i = find_block(p->blocks, base);
  struct process *target = proc_lookup(target_pid);
  if (i < 0 || target == nullptr || target == p) {
    umem_unlock();
    return -1;
  }
  ublock *b;
  vec_ublock_ptr_get(p->blocks, (uint32_t)i, &b);
  if (find_block(target->shared_in, base) >= 0) {
    umem_unlock();
    return -1; // already shared to this process
  }
  vec_process_ptr_push(b->sharers, &target);
  vec_ublock_ptr_push(target->shared_in, &b);
  as_flag(target->as, b->base, b->base + ublock_bytes(b), prot | PAGE_U);
  as_flush(target->as);
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
  remove_proc(b->sharers, p);
  as_flag(p->as, b->base, b->base + ublock_bytes(b), PAGE_KERNEL_PRISTINE);
  as_flush(p->as);
  umem_unlock();
  return 0;
}

// ---------------------------------------------------------------------------
// Process exit
// ---------------------------------------------------------------------------

void umem_destroy_process(struct process *p) {
  umem_lock();

  // Drop shared-in memberships. No flag/flush of p->as: it is drained
  // (no CPU has it current) and about to be as_free'd.
  while (vec_ublock_ptr_len(p->shared_in) > 0) {
    ublock *b;
    vec_ublock_ptr_get(p->shared_in, 0, &b);
    vec_ublock_ptr_swap_and_pop(p->shared_in, 0);
    remove_proc(b->sharers, p);
  }

  // Owner death revokes: every sharer's view is torn down and the blocks
  // go back to the buddy.
  while (vec_ublock_ptr_len(p->blocks) > 0) {
    ublock *b;
    vec_ublock_ptr_get(p->blocks, 0, &b);
    vec_ublock_ptr_swap_and_pop(p->blocks, 0);
    block_release_locked(b, true);
  }

  remove_proc(g_procs, p);
  umem_unlock();

  vec_ublock_ptr_delete(&p->blocks);
  vec_ublock_ptr_delete(&p->shared_in);
}
