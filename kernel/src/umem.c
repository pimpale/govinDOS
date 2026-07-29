#include "umem.h"

#include <stdatomic.h>

#include "allocator.h"
#include "buddy_allocator/buddy_allocator.h"
#include "channel.h"
#include "debug.h"
#include "paging.h"
#include "platform_mem.h"
#include "process.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"
#include "uaccess.h"

// ---------------------------------------------------------------------------
// The lock hierarchy (contract in umem.h)
// ---------------------------------------------------------------------------
//
// g_umem is what remains of the old single umem lock: the control plane.
// It still serializes every ownership-graph mutation — cross-process
// operations (share, revoke, owner-exit) would otherwise need
// multi-process lock ordering, and serializing them is the honest choice
// at this scale. What it no longer covers is the IPC data plane
// (ring-local CQ locks in channel.c) and the long tail of revocation (the
// combined flush round in umem_release_finish, which runs with no locks
// held at all).

static struct svclock g_umem;

void umem_lock(void) { svclock_lock(&g_umem); }
void umem_unlock(void) { svclock_unlock(&g_umem); }

void umem_proc_lock(struct process *p) { svclock_lock(&p->ulock); }
void umem_proc_unlock(struct process *p) { svclock_unlock(&p->ulock); }

// ---------------------------------------------------------------------------
// Registry (under g_umem)
// ---------------------------------------------------------------------------

static llrb_pid_process *g_procs;
static llrb_base_ublock *g_ublocks;

static struct process *proc_lookup(uint64_t pid) {
  struct process *p;
  if (!llrb_pid_process_get(g_procs, &pid, &p))
    return nullptr;
  return p == nullptr || process_is_dead(p) ? nullptr : p;
}

static struct process *proc_lookup_any(uint64_t pid) {
  struct process *p;
  return llrb_pid_process_get(g_procs, &pid, &p) ? p : nullptr;
}

// ---------------------------------------------------------------------------
// Ordered-index helpers
// ---------------------------------------------------------------------------

static inline uint64_t ublock_bytes(const struct ublock *b) {
  return b->bytes;
}

ublock *umem_owned_locked(struct process *p, uint64_t base) {
  ublock *b;
  if (!llrb_base_block_get_ref(p->blocks, &base, &b))
    return nullptr;
  return b;
}

ublock *umem_view_locked(struct process *p, uint64_t addr, uint64_t len) {
  uint64_t end;
  if (len == 0 || __builtin_add_overflow(addr, len, &end))
    return nullptr;
  ublock *b = nullptr;
  ublock owned;
  if (llrb_base_block_floor(p->blocks, &addr, nullptr, &owned) &&
      end <= owned.base + ublock_bytes(&owned)) {
    // Recover the stable inline value rather than return the floor copy.
    asserts(llrb_base_block_get_ref(p->blocks, &owned.base, &b),
            "umem: floor block disappeared");
    return b;
  }
  share_edge *e = nullptr;
  (void)llrb_base_edge_floor(p->views, &addr, nullptr, &e);
  return e != nullptr && end <= e->block->base + ublock_bytes(e->block)
             ? e->block
             : nullptr;
}

static void pending_link(share_edge *e) {
  struct process *p = e->to;
  asserts(!e->notified && e->pending_prev == nullptr &&
              e->pending_next == nullptr,
          "umem: pending edge linked twice");
  e->pending_prev = p->unnotified_tail;
  if (p->unnotified_tail != nullptr)
    p->unnotified_tail->pending_next = e;
  else
    p->unnotified_head = e;
  p->unnotified_tail = e;
}

void umem_edge_pending_unlink_locked(share_edge *e) {
  if (e->notified)
    return;
  struct process *p = e->to;
  if (e->pending_prev != nullptr)
    e->pending_prev->pending_next = e->pending_next;
  else
    p->unnotified_head = e->pending_next;
  if (e->pending_next != nullptr)
    e->pending_next->pending_prev = e->pending_prev;
  else
    p->unnotified_tail = e->pending_prev;
  e->pending_prev = nullptr;
  e->pending_next = nullptr;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void umem_init(void) {
  asserts(g_procs == nullptr, "umem_init: called twice");
  asserts(llrb_pid_process_new(&g_procs), "umem: pid tree alloc failed");
  asserts(llrb_base_ublock_new(&g_ublocks), "umem: block tree alloc failed");
}

void umem_process_register(struct process *p) {
  asserts(g_procs != nullptr, "umem: not initialized");
  asserts(llrb_base_block_new(&p->blocks),
          "umem: process block tree alloc failed");
  asserts(llrb_base_edge_new(&p->views), "umem: view tree alloc failed");
  umem_lock();
  asserts(llrb_pid_process_insert(g_procs, &p->pid, &p),
          "umem: duplicate pid or pid node alloc failed");
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

  uint64_t page_id = 0;
  spinlock_lock(&g_allocator_lock);
  buddy_status_t s = buddy_page_alloc(g_allocator, 1ull << order, &page_id);
  spinlock_unlock(&g_allocator_lock);
  if (s != BUDDY_STATUS_SUCCESS) {
    return nullptr;
  }
  uint8_t *base = (uint8_t *)(page_id * PAGE_SIZE);

  // Mandatory: blocks recycle across processes and users.
  memset(base, 0, bytes);

  ublock value = {.base = (uint64_t)base,
                  .order = order,
                  .bytes = bytes,
                  .backing = UBLOCK_RAM,
                  .kernel_flags = PAGE_KERNEL_PRISTINE,
                  .delegatable = true,
                  .owner = p};
  if (!llrb_pid_edge_new(&value.sharers)) {
    spinlock_lock(&g_allocator_lock);
    (void)buddy_page_free(g_allocator, page_id);
    spinlock_unlock(&g_allocator_lock);
    return nullptr;
  }
  llrb_base_block_node *owned_node =
      slab_llrb_base_block_node_malloc(sizeof(*owned_node));
  if (owned_node == nullptr) {
    llrb_pid_edge_delete(&value.sharers);
    spinlock_lock(&g_allocator_lock);
    (void)buddy_page_free(g_allocator, page_id);
    spinlock_unlock(&g_allocator_lock);
    return nullptr;
  }

  // The block is invisible to free/protect until pushed, so the flag can
  // happen outside any lock; flush before the base escapes to the caller.
  as_flag(p->as, value.base, value.base + bytes, prot | PAGE_U);
  as_flush(p->as);

  // The process-owned node is the metadata allocation. g_ublocks is only
  // a secondary pointer index into that stable inline value.
  umem_lock();
  umem_proc_lock(p);
  asserts(llrb_base_block_insert_node(p->blocks, &value.base, &value,
                                      owned_node),
          "umem: owned block insertion failed");
  ublock *b;
  asserts(llrb_base_block_get_ref(p->blocks, &value.base, &b),
          "umem: inserted block missing");
  umem_proc_unlock(p);
  asserts(llrb_base_ublock_insert(g_ublocks, &b->base, &b),
          "umem: block index insertion failed");
  umem_unlock();
  return base;
}

uint64_t umem_size(struct process *p, uint64_t base) {
  umem_proc_lock(p);
  ublock *b = umem_owned_locked(p, base);
  uint64_t bytes = b == nullptr ? 0 : ublock_bytes(b);
  umem_proc_unlock(p);
  return bytes;
}

uint64_t umem_map_device_locked(struct process *p, uint64_t base, uint64_t len,
                                uint32_t flags) {
  paging_flags_t kernel_flags;
  bool delegatable;
  if (!platform_mem_validate_device(base, len, flags, &kernel_flags,
                                    &delegatable))
    return SYSERR_INVAL;
  uint64_t end = base + len;
  paging_flags_t device_flags = PAGE_R;
  if (flags & VM_DEVICE_WRITE)
    device_flags |= PAGE_W;
  if (!(flags & VM_DEVICE_FIRMWARE))
    device_flags |= PAGE_UC;

  llrb_base_ublock_iter blocks;
  llrb_base_ublock_iter_begin(g_ublocks, &blocks);
  ublock *other;
  while (llrb_base_ublock_iter_next(&blocks, nullptr, &other)) {
    if (other->backing == UBLOCK_DEVICE && base < other->base + other->bytes &&
        other->base < end)
      return SYSERR_EXIST;
  }

  // Fix the kernel-only alias in every extant tree before making a user
  // alias visible. Device cache types stay fixed after first registration;
  // later clones inherit the same leaf from g_as_kernel.
  as_flag(g_as_kernel, base, end, kernel_flags);
  as_flush(g_as_kernel);
  llrb_pid_process_iter iter;
  llrb_pid_process_iter_begin(g_procs, &iter);
  struct process *q;
  while (llrb_pid_process_iter_next(&iter, nullptr, &q)) {
    if (q->as != nullptr) {
      as_flag(q->as, base, end, kernel_flags);
      as_flush(q->as);
    }
  }

  ublock value = {.base = base,
                  .bytes = len,
                  .backing = UBLOCK_DEVICE,
                  .device_flags = device_flags,
                  .kernel_flags = kernel_flags,
                  .delegatable = delegatable,
                  .owner = p};
  if (!llrb_pid_edge_new(&value.sharers))
    return SYSERR_NOMEM;
  llrb_base_block_node *owned_node =
      slab_llrb_base_block_node_malloc(sizeof(*owned_node));
  if (owned_node == nullptr) {
    llrb_pid_edge_delete(&value.sharers);
    return SYSERR_NOMEM;
  }
  as_flag(p->as, base, end, device_flags | PAGE_U);
  as_flush(p->as);
  umem_proc_lock(p);
  if (!llrb_base_block_insert_node(p->blocks, &base, &value, owned_node)) {
    umem_proc_unlock(p);
    llrb_pid_edge_delete(&value.sharers);
    slab_llrb_base_block_node_free(owned_node);
    as_flag(p->as, base, end, kernel_flags);
    as_flush(p->as);
    return SYSERR_NOMEM;
  }
  ublock *b;
  asserts(llrb_base_block_get_ref(p->blocks, &base, &b),
          "umem: inserted device block missing");
  umem_proc_unlock(p);
  asserts(llrb_base_ublock_insert(g_ublocks, &b->base, &b),
          "umem: device block index insertion failed");
  return 0;
}

uint64_t umem_map_device(struct process *p, uint64_t base, uint64_t len,
                         uint32_t flags) {
  umem_lock();
  uint64_t rc = umem_map_device_locked(p, base, len, flags);
  umem_unlock();
  return rc;
}

// Phase one of freeing a block: everything that needs g_umem. Caller
// holds g_umem and has already unlinked b from its owner's tree. The
// bounded free gate guarantees there are no sharer views. This tears down
// channel state, restores the owner view to pristine (dirty-marking only),
// and pins the viewing AS into `rel`.
// After this returns, nothing can reach b — the expensive tail (the
// flush round and the buddy return) runs in umem_release_finish with no
// locks held.
static void block_release_prepare(ublock *b, struct umem_release *rel) {
  uint64_t end = b->base + ublock_bytes(b);
  asserts(llrb_pid_edge_len(b->sharers) == 0,
          "umem: release with sharers");

  // A ring endpoint dies with its block. Parked futex waiters are not
  // notified — their recovery is their deadline, and a later touch of
  // the block is the ordinary revocation death (futex-design.md §5).
  channel_ring_destroy(b);

  rel->b = b;
  rel->nases = 1;
  rel->ases = malloc(rel->nases * sizeof(*rel->ases));
  asserts(rel->ases != nullptr, "umem: release ctx alloc failed");

  as_flag(b->owner->as, b->base, end, b->kernel_flags);
  as_pin(b->owner->as);
  rel->ases[0] = b->owner->as;
}

void umem_release_finish(struct umem_release *rel) {
  if (rel->b == nullptr) {
    return;
  }
  ublock *b = rel->b;

  // Pristine everywhere + one combined shootdown round: safe to recycle.
  // The pins keep a concurrently-destroyed sharer's AS alive under the
  // flush; PROC_DESTROY treats pins != 0 as "still draining".
  as_flush_multi(rel->ases, rel->nases);
  for (uint32_t i = 0; i < rel->nases; i++) {
    as_unpin(rel->ases[i]);
  }
  free(rel->ases);

  if (b->backing == UBLOCK_RAM) {
    spinlock_lock(&g_allocator_lock);
    buddy_status_t s = buddy_page_free(g_allocator, b->base / PAGE_SIZE);
    spinlock_unlock(&g_allocator_lock);
    asserts(s == BUDDY_STATUS_SUCCESS, "umem: buddy rejected block free");
  }

  llrb_pid_edge_delete(&b->sharers);
  slab_llrb_base_block_node_free(rel->owned_node);
  rel->b = nullptr;
  rel->owned_node = nullptr;
}

static int free_locked(struct process *p, uint64_t base,
                       struct umem_release *rel) {
  umem_proc_lock(p);
  ublock *b;
  if (!llrb_base_block_get_ref(p->blocks, &base, &b)) {
    umem_proc_unlock(p);
    return -1;
  }
  // A single bounded transaction: fail while anything remains attached
  // rather than driving its removal. Orderly teardown drains sharers
  // first (sentinel -> wake -> peers DROPSHARE, VM_UNSHARE coercing).
  if (b->dma_maps != nullptr || atomic_load(&b->thread_pins) != 0 ||
      llrb_pid_edge_len(b->sharers) != 0 ||
      !channel_block_destroyable(b)) {
    umem_proc_unlock(p);
    return (int)SYSERR_EXIST;
  }
  ublock removed_value;
  llrb_base_block_node *owned_node;
  asserts(llrb_base_block_extract(p->blocks, &base, &removed_value,
                                  &owned_node),
          "umem: owned block extraction failed");
  umem_proc_unlock(p);
  ublock *removed;
  asserts(llrb_base_ublock_remove(g_ublocks, &b->base, &removed) &&
              removed == b,
          "umem: block index removal failed");
  // extract preserves the node and therefore the address of its inline value.
  asserts(&owned_node->value == b, "umem: block identity changed");
  rel->owned_node = owned_node;
  block_release_prepare(b, rel);
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
  // Flag + flush under the list lock: SYS_FUTEX_WAIT loads its user word
  // under the same lock, so a guard (prot == 0) can never yank a mapping
  // between wait's user_range_ok and its load.
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, len);
  if (b != nullptr && atomic_load(&b->thread_pins) != 0) {
    umem_proc_unlock(p);
    return -1;
  }
  // A kernel channel's CQ is written from borrowed contexts, including IRQ
  // context where a kernel-mode page fault is fatal. Scheme creation holds
  // this same list lock through b->ring publication, so this test cannot
  // race a protect into the creation window.
  if (b == nullptr || b->ring != nullptr ||
      (b->backing == UBLOCK_DEVICE &&
       ((prot & ~b->device_flags) != 0 || (prot & PAGE_X)))) {
    umem_proc_unlock(p);
    return -1;
  }
  paging_flags_t view = prot == 0 ? 0 : (prot | PAGE_U);
  if (b->backing == UBLOCK_DEVICE)
    view |= b->device_flags & PAGE_CACHE_MASK;
  as_flag(p->as, base, end, view);
  as_flush(p->as);
  umem_proc_unlock(p);
  return 0;
}

uint64_t umem_share(struct process *p, uint64_t base, uint64_t target_pid,
                    paging_flags_t prot) {
  llrb_pid_edge_node *edge_node =
      slab_llrb_pid_edge_node_malloc(sizeof(*edge_node));
  llrb_base_edge_node *view_node =
      slab_llrb_base_edge_node_malloc(sizeof(*view_node));
  if (edge_node == nullptr || view_node == nullptr) {
    slab_llrb_pid_edge_node_free(edge_node);
    slab_llrb_base_edge_node_free(view_node);
    return SYSERR_NOMEM;
  }
  umem_lock();
  umem_proc_lock(p);
  ublock *b = umem_owned_locked(p, base);
  struct process *target = proc_lookup(target_pid);
  if (b == nullptr || target == nullptr || target == p || b->ring != nullptr ||
      atomic_load(&b->thread_pins) != 0 || !b->delegatable ||
      (b->backing == UBLOCK_DEVICE &&
       ((prot & ~b->device_flags) != 0 || (prot & PAGE_X)))) {
    umem_proc_unlock(p);
    umem_unlock();
    slab_llrb_pid_edge_node_free(edge_node);
    slab_llrb_base_edge_node_free(view_node);
    return SYSERR_INVAL;
  }
  share_edge *existing;
  if (llrb_pid_edge_get_ref(b->sharers, &target->pid, &existing)) {
    umem_proc_unlock(p);
    umem_unlock();
    slab_llrb_pid_edge_node_free(edge_node);
    slab_llrb_base_edge_node_free(view_node);
    return SYSERR_EXIST;
  }
  share_edge value = {.block = b, .to = target};
  asserts(llrb_pid_edge_insert_node(b->sharers, &target->pid, &value,
                                    edge_node),
          "umem: share edge insertion failed");
  share_edge *e;
  asserts(llrb_pid_edge_get_ref(b->sharers, &target->pid, &e),
          "umem: inserted share edge missing");
  pending_link(e);
  umem_proc_unlock(p);
  // Topology changes are not identity changes: waits are address-keyed,
  // so parked waiters are unaffected by a new edge and any number of
  // sharers is waitable (a count-capped wake is the bound).

  umem_proc_lock(target);
  asserts(llrb_base_edge_insert_node(target->views, &b->base, &e, view_node),
          "umem: view index insertion failed");
  umem_proc_unlock(target);
  paging_flags_t target_flags = prot | PAGE_U;
  if (b->backing == UBLOCK_DEVICE)
    target_flags |= b->device_flags & PAGE_CACHE_MASK;
  as_flag(target->as, b->base, b->base + ublock_bytes(b), target_flags);
  as_flush(target->as);

  // Announce on the target's shares channel (or leave the edge's notified
  // bit clear for replay when the channel exists / has room).
  channel_edge_notify(b, e);
  umem_unlock();
  return 0;
}

static ublock *find_owned_global_locked(uint64_t base) {
  ublock *b;
  return llrb_base_ublock_get(g_ublocks, &base, &b) ? b : nullptr;
}

static bool resource_authorized(struct process *caller,
                                struct process *owner) {
  return caller == owner ||
         process_reaper_authorized_locked(caller, owner);
}

ublock *umem_resource_block_locked(struct process *caller, uint64_t base) {
  ublock *b = find_owned_global_locked(base);
  return b != nullptr && resource_authorized(caller, b->owner) ? b : nullptr;
}

uint64_t umem_free(struct process *caller, uint64_t base) {
  struct umem_release rel = {0};
  umem_lock();
  ublock *b = find_owned_global_locked(base);
  if (b == nullptr || !resource_authorized(caller, b->owner)) {
    umem_unlock();
    return SYSERR_PERM;
  }
  int rc = free_locked(b->owner, base, &rel);
  umem_unlock();
  umem_release_finish(&rel);
  return rc == 0 ? 0 : (uint64_t)(rc == (int)SYSERR_EXIST
                                      ? SYSERR_EXIST
                                      : SYSERR_PERM);
}

uint64_t umem_dropshare(struct process *caller, uint64_t base, uint64_t pid) {
  umem_lock();
  struct process *target =
      pid == 0 || pid == caller->pid ? caller : proc_lookup_any(pid);
  if (target == nullptr ||
      (target != caller &&
       !process_reaper_authorized_locked(caller, target))) {
    umem_unlock();
    return SYSERR_PERM;
  }
  umem_proc_lock(target);
  share_edge *e;
  if (!llrb_base_edge_get(target->views, &base, &e)) {
    umem_proc_unlock(target);
    umem_unlock();
    return SYSERR_INVAL;
  }
  ublock *b = e->block;
  share_edge *removed_view;
  asserts(llrb_base_edge_remove(target->views, &base, &removed_view) &&
              removed_view == e,
          "umem: view index removal failed");
  umem_proc_unlock(target);
  umem_edge_pending_unlink_locked(e);
  share_edge removed_edge;
  asserts(llrb_pid_edge_remove(b->sharers, &target->pid, &removed_edge),
          "umem: share edge removal failed");
  as_flag(target->as, b->base, b->base + ublock_bytes(b), b->kernel_flags);
  as_flush(target->as);
  umem_unlock();
  return 0;
}

uint64_t umem_unshare(struct process *caller, uint64_t base, uint64_t pid) {
  umem_lock();
  ublock *b = find_owned_global_locked(base);
  if (b == nullptr || !resource_authorized(caller, b->owner)) {
    umem_unlock();
    return SYSERR_PERM;
  }
  struct process *target = proc_lookup_any(pid);
  share_edge *e;
  if (target == nullptr ||
      !llrb_pid_edge_get_ref(b->sharers, &pid, &e)) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  umem_proc_lock(target);
  share_edge *removed_view;
  asserts(llrb_base_edge_remove(target->views, &base, &removed_view) &&
              removed_view == e,
          "umem: view index removal failed");
  umem_proc_unlock(target);
  umem_edge_pending_unlink_locked(e);
  share_edge removed_edge;
  asserts(llrb_pid_edge_remove(b->sharers, &pid, &removed_edge),
          "umem: share edge removal failed");
  as_flag(target->as, b->base, b->base + ublock_bytes(b), b->kernel_flags);
  as_flush(target->as);
  umem_unlock();
  return 0;
}

static struct process *enum_subject_locked(struct process *caller,
                                           uint64_t pid) {
  if (pid == 0 || pid == caller->pid)
    return caller;
  struct process *target = proc_lookup_any(pid);
  return target != nullptr &&
                 process_reaper_authorized_locked(caller, target)
             ? target
             : nullptr;
}

uint64_t umem_enum_blocks(struct process *caller, uint64_t pid, uint64_t buf,
                          uint64_t cap, uint64_t after) {
  if (cap == 0 || cap > VM_ENUM_BATCH)
    return SYSERR_INVAL;
  uint64_t values[VM_ENUM_BATCH], count = 0;
  umem_lock();
  struct process *target = enum_subject_locked(caller, pid);
  if (target == nullptr) {
    umem_unlock();
    return SYSERR_PERM;
  }
  umem_proc_lock(target);
  llrb_base_block_iter iter;
  llrb_base_block_iter_lower_bound(target->blocks, &after, &iter);
  uint64_t key;
  while (count < cap &&
         llrb_base_block_iter_next(&iter, &key, nullptr)) {
    if (key > after)
      values[count++] = key;
  }
  umem_proc_unlock(target);
  uint64_t rc = umem_enum_copyout_locked(caller, buf, cap, values, count);
  umem_unlock();
  return rc;
}

uint64_t umem_enum_views(struct process *caller, uint64_t pid, uint64_t buf,
                         uint64_t cap, uint64_t after) {
  if (cap == 0 || cap > VM_ENUM_BATCH)
    return SYSERR_INVAL;
  uint64_t values[VM_ENUM_BATCH], count = 0;
  umem_lock();
  struct process *target = enum_subject_locked(caller, pid);
  if (target == nullptr) {
    umem_unlock();
    return SYSERR_PERM;
  }
  umem_proc_lock(target);
  llrb_base_edge_iter iter;
  llrb_base_edge_iter_lower_bound(target->views, &after, &iter);
  uint64_t key;
  while (count < cap &&
         llrb_base_edge_iter_next(&iter, &key, nullptr)) {
    if (key > after)
      values[count++] = key;
  }
  umem_proc_unlock(target);
  uint64_t rc = umem_enum_copyout_locked(caller, buf, cap, values, count);
  umem_unlock();
  return rc;
}

uint64_t umem_enum_sharers(struct process *caller, uint64_t base,
                           uint64_t buf, uint64_t cap, uint64_t after) {
  if (cap == 0 || cap > VM_ENUM_BATCH)
    return SYSERR_INVAL;
  uint64_t values[VM_ENUM_BATCH], count = 0;
  umem_lock();
  ublock *b = find_owned_global_locked(base);
  if (b == nullptr || !resource_authorized(caller, b->owner)) {
    umem_unlock();
    return SYSERR_PERM;
  }
  llrb_pid_edge_iter iter;
  llrb_pid_edge_iter_lower_bound(b->sharers, &after, &iter);
  uint64_t key;
  while (count < cap && llrb_pid_edge_iter_next(&iter, &key, nullptr)) {
    if (key > after)
      values[count++] = key;
  }
  uint64_t rc = umem_enum_copyout_locked(caller, buf, cap, values, count);
  umem_unlock();
  return rc;
}

void umem_process_finish_locked(struct process *p) {
  asserts(llrb_base_block_len(p->blocks) == 0 &&
              llrb_base_edge_len(p->views) == 0,
          "umem: process finish with resources left");
  llrb_base_block_delete(&p->blocks);
  llrb_base_edge_delete(&p->views);
}

// ---------------------------------------------------------------------------
// Ownership transfer (SYS_VM_MOVE; tree checks live in process.c)
// ---------------------------------------------------------------------------

uint64_t umem_move_locked(ublock *b, struct process *from, struct process *to,
                          bool src_as_live) {
  if (b->dma_maps != nullptr || atomic_load(&b->thread_pins) != 0)
    return SYSERR_EXIST;
  if (!b->delegatable)
    return SYSERR_INVAL;
  uint64_t bytes = ublock_bytes(b);
  // Both endpoints may have live threads. g_umem serializes their
  // control-plane operations while the page-table transition and
  // as_flush_multi make the change coherent to userspace on every CPU.
  // Parked waiters are address-keyed and unaffected by the ownership move.
  umem_proc_lock(from);
  ublock removed;
  llrb_base_block_node *node;
  asserts(llrb_base_block_extract(from->blocks, &b->base, &removed, &node) &&
              &node->value == b,
          "umem: moved block missing from owner");
  umem_proc_unlock(from);
  umem_proc_lock(to);
  share_edge *old_view = nullptr;
  bool had_view = llrb_base_edge_remove(to->views, &b->base, &old_view);
  if (had_view) {
    // The receiver already had a shared-in view; the owner view below
    // replaces it (and its edge).
  }
  umem_proc_unlock(to);

  if (had_view) {
    umem_edge_pending_unlink_locked(old_view);
    share_edge removed_edge;
    asserts(llrb_pid_edge_remove(b->sharers, &to->pid, &removed_edge),
            "umem: moved view edge missing");
  }
  b->owner = to;

  if (src_as_live) {
    as_flag(from->as, b->base, b->base + bytes, b->kernel_flags);
  }
  paging_flags_t moved = PAGE_R | PAGE_W | PAGE_U;
  if (b->backing == UBLOCK_DEVICE)
    moved = b->device_flags | PAGE_U;
  as_flag(to->as, b->base, b->base + bytes, moved);
  struct address_space *ases[2] = {to->as, from->as};
  as_flush_multi(ases, src_as_live ? 2 : 1);

  umem_proc_lock(to);
  asserts(llrb_base_block_insert_node(to->blocks, &b->base, b, node),
          "umem: moved block collided");
  umem_proc_unlock(to);
  return 0;
}

// ---------------------------------------------------------------------------
// Registry maintenance (process.c, under g_umem)
// ---------------------------------------------------------------------------

struct process *umem_proc_lookup_locked(uint64_t pid) {
  return proc_lookup(pid);
}

struct process *umem_proc_lookup_any_locked(uint64_t pid) {
  return proc_lookup_any(pid);
}

void umem_proc_unregister_locked(struct process *p) {
  struct process *removed;
  asserts(llrb_pid_process_remove(g_procs, &p->pid, &removed) && removed == p,
          "umem: process missing from pid tree");
}

uint64_t umem_enum_copyout_locked(struct process *caller, uint64_t buf,
                                  uint64_t cap, const uint64_t *values,
                                  uint64_t count) {
  if (cap == 0 || cap > VM_ENUM_BATCH || count > cap ||
      cap > UINT64_MAX / sizeof(uint64_t))
    return SYSERR_INVAL;
  uint64_t bytes = cap * sizeof(uint64_t);
  umem_proc_lock(caller);
  ublock *b = umem_view_locked(caller, buf, bytes);
  bool ok = b != nullptr && user_range_ok(caller, buf, bytes, true);
  if (!ok) {
    umem_proc_unlock(caller);
    return SYSERR_FAULT;
  }
  memcpy((void *)buf, values, count * sizeof(uint64_t));
  umem_proc_unlock(caller);
  return count;
}
