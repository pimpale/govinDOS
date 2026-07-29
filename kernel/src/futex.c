#include "futex.h"

#include <stdatomic.h>
#include <stdint.h>

#include "channel.h"
#include "debug.h"
#include "hash.h"
#include "process.h"
#include "spinlock.h"
#include "syscall.h"
#include "thread.h"
#include "timer.h"
#include "uaccess.h"
#include "umem.h"

// The bucket table (futex-design.md §3). Plain spinlocks, not svclocks:
// no futex path is ever held across as_flush, and the wake paths run in
// interrupt context, where a shootdown-servicing lock is forbidden.
// Lock order: list locks (p->ulock) and ring CQ locks rank above buckets;
// buckets rank above the per-CPU timer locks and the scheduler lock.
//
// Interim paging discipline: until page-table pages are retired after
// the shootdown (memory-design.md), the park and requeue paths hold the
// caller's list lock across the view check, the word load, and the
// insert. umem_protect flags under that lock, and a revoking owner must
// take it to unlink the caller's view, so check-to-load cannot race a
// revoke-and-recycle.

#define FUTEX_NBUCKETS_LOG2 10
#define FUTEX_NBUCKETS (1u << FUTEX_NBUCKETS_LOG2)

struct futex_bucket {
  struct spinlock lock;
  llrb_futex *waiters;
};

static struct futex_bucket g_futex[FUTEX_NBUCKETS];

// Global monotonic park sequence: the FIFO tiebreaker within an address.
static _Atomic uint64_t g_futex_seq = 1;

static struct futex_bucket *bucket_of(uint64_t addr) {
  // Hash the word index; low bits alone would put every word of one
  // block in one bucket.
  return &g_futex[hash_fib(addr / 4, FUTEX_NBUCKETS_LOG2)];
}

void futex_init(void) {
  for (uint32_t i = 0; i < FUTEX_NBUCKETS; i++) {
    spinlock_init(&g_futex[i].lock);
    asserts(llrb_futex_new(&g_futex[i].waiters),
            "futex: bucket tree alloc failed");
  }
}

bool futex_try_claim(struct thread *t) {
  for (;;) {
    enum futex_state s =
        atomic_load_explicit(&t->wake_state, memory_order_acquire);
    if (s == FUTEX_MOVING) {
      // Requeue's relink window: strictly pointer work under two bucket
      // locks, so spinning here is bounded even from interrupt context.
      __asm__ volatile("pause");
      continue;
    }
    if (s != FUTEX_PARKED) {
      return false;
    }
    enum futex_state expected = FUTEX_PARKED;
    if (atomic_compare_exchange_weak_explicit(
            &t->wake_state, &expected, FUTEX_CLAIMED, memory_order_acq_rel,
            memory_order_acquire)) {
      return true;
    }
  }
}

// Dispose of a claimed thread: remove its deadline entry, then either
// deliver `result` and release it, or publish REAPABLE for a dead
// process's thread (reap frees it; making it runnable would dispatch
// into a dying AS). After this returns the caller may not touch t.
static void finish_claimed(struct thread *t, uint64_t result) {
  timer_deadline_cancel(t);
  if (process_is_dead(t->proc)) {
    atomic_store_explicit(&t->wake_state, FUTEX_REAPABLE,
                          memory_order_release);
    return;
  }
  thread_deliver_wait_result(t, result);
  thread_unblock_claimed(t);
}

// Claim and detach up to `cap` waiters parked on exactly `addr`, FIFO.
// Bucket held. Removal invalidates the template's iterators, so each
// round re-seeks the lower bound past the last visited sequence. A
// waiter lost to a racing timeout or reap is skipped — its wake is that
// winner's result, not this one's (an expiry-claimed thread's node can
// still be in the tree for the winner to remove).
static uint32_t detach_batch_locked(struct futex_bucket *bk, uint64_t addr,
                                    uint32_t cap, struct thread **batch) {
  uint32_t n = 0;
  uint64_t next_seq = 0;
  while (n < cap) {
    llrb_futex_iter iter;
    struct futex_key key;
    thread_ptr t;
    struct futex_key seek = {.address = addr, .seq = next_seq};
    llrb_futex_iter_lower_bound(bk->waiters, &seek, &iter);
    if (!llrb_futex_iter_next(&iter, &key, &t) || key.address != addr) {
      break;
    }
    next_seq = key.seq + 1;
    enum futex_state expected = FUTEX_PARKED;
    if (!atomic_compare_exchange_strong_explicit(
            &t->wake_state, &expected, FUTEX_CLAIMED, memory_order_acq_rel,
            memory_order_acquire)) {
      continue;
    }
    thread_ptr removed;
    asserts(llrb_futex_remove(bk->waiters, &key, &removed) && removed == t,
            "futex: bucket tree lost claimed waiter");
    batch[n++] = t;
  }
  return n;
}

// Pop under the lock, unblock outside it: thread_unblock_claimed spins
// on the target's in-flight context save, and a batch must not hold the
// bucket across up to FUTEX_WAKE_BATCH such spins.
static uint64_t wake_batch(uint64_t addr, uint64_t count) {
  uint32_t cap =
      count < FUTEX_WAKE_BATCH ? (uint32_t)count : FUTEX_WAKE_BATCH;
  struct thread *batch[FUTEX_WAKE_BATCH];
  struct futex_bucket *bk = bucket_of(addr);
  spinlock_lock(&bk->lock);
  uint32_t n = detach_batch_locked(bk, addr, cap, batch);
  spinlock_unlock(&bk->lock);
  for (uint32_t i = 0; i < n; i++) {
    finish_claimed(batch[i], 0);
  }
  return n;
}

void futex_wake_one(uint64_t addr) { wake_batch(addr, 1); }

void futex_publish_wake(uint64_t addr, uint32_t value) {
  struct thread *batch[1];
  struct futex_bucket *bk = bucket_of(addr);
  spinlock_lock(&bk->lock);
  // Publishing under the bucket makes the transition lossless: a thread
  // whose compare-and-park runs after this sees the new value and never
  // parks, and one parked before is in the tree below.
  atomic_store_explicit((volatile _Atomic uint32_t *)addr, value,
                        memory_order_release);
  uint32_t n = detach_batch_locked(bk, addr, 1, batch);
  spinlock_unlock(&bk->lock);
  if (n != 0) {
    finish_claimed(batch[0], 0);
  }
}

void futex_expire_claimed(struct thread *t) {
  struct futex_bucket *bk = bucket_of(t->wait_key.address);
  spinlock_lock(&bk->lock);
  thread_ptr removed;
  asserts(llrb_futex_remove(bk->waiters, &t->wait_key, &removed) &&
              removed == t,
          "futex: bucket tree lost expired waiter");
  spinlock_unlock(&bk->lock);
  finish_claimed(t, SYSERR_TIMEDOUT);
}

void futex_reap_claimed(struct thread *t) {
  struct futex_bucket *bk = bucket_of(t->wait_key.address);
  spinlock_lock(&bk->lock);
  thread_ptr removed;
  asserts(llrb_futex_remove(bk->waiters, &t->wait_key, &removed) &&
              removed == t,
          "futex: bucket tree lost reaped waiter");
  spinlock_unlock(&bk->lock);
  timer_deadline_cancel(t);
}

// ---------------------------------------------------------------------------
// Syscall backends
// ---------------------------------------------------------------------------

uint64_t futex_sys_wake(struct thread *curr, uint64_t addr, uint64_t count) {
  struct process *p = curr->proc;
  // The wake namespace is gated by view membership: an address the
  // caller was never granted wakes nothing and reads nothing.
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, addr, 1);
  bool is_ring = b != nullptr && b->ring != nullptr;
  umem_proc_unlock(p);
  if (b == nullptr) {
    return SYSERR_INVAL;
  }
  if (is_ring) {
    // The old doorbell: drain and execute the SQ in the caller's context,
    // run the scheme's replay, ignore count. The word is never read.
    return channel_ring_drain(curr, addr);
  }
  return wake_batch(addr, count);
}

uint64_t futex_sys_wait(struct thread *curr, uint64_t addr,
                        uint64_t expected, uint64_t wake_addr,
                        uint64_t deadline) {
  if (addr % 4 != 0) {
    return SYSERR_INVAL;
  }
  // The fused wake completes and releases everything it touched before
  // the park begins, so no path holds two bucket locks here. Failure is
  // ignored: the peer being gone (a live case during teardown) is
  // exactly when the caller most needs its own park to run.
  if (wake_addr != 0) {
    futex_sys_wake(curr, wake_addr, 1);
  }

  struct process *p = curr->proc;
  umem_proc_lock(p);
  // A kill can land between the dispatcher's checkpoint and here. An
  // effectively dead process must not install a new waiter.
  if (process_is_dead(p)) {
    umem_proc_unlock(p);
    uthread_park_exit();
  }
  // Authorization and atomicity in one: without a view check,
  // compare-and-park is a 32-bit equality oracle over every word in the
  // SASOS; the list-lock hold closes the check-to-load window (interim
  // discipline, header comment).
  if (!user_range_ok(p, addr, 4, false)) {
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }

  struct futex_bucket *bk = bucket_of(addr);
  spinlock_lock(&bk->lock);
  // The word is untrusted: only compared, never interpreted.
  uint32_t word = *(volatile uint32_t *)addr;
  if (word != (uint32_t)expected) {
    spinlock_unlock(&bk->lock);
    umem_proc_unlock(p);
    return SYSERR_AGAIN;
  }
  struct futex_key key = {
      .address = addr,
      .seq = atomic_fetch_add_explicit(&g_futex_seq, 1,
                                       memory_order_relaxed),
  };
  thread_ptr value = curr;
  // Fresh sequence numbers never collide, so false is allocation failure.
  if (!llrb_futex_insert(bk->waiters, &key, &value)) {
    spinlock_unlock(&bk->lock);
    umem_proc_unlock(p);
    return SYSERR_NOMEM;
  }
  curr->wait_key = key;
  curr->deadline = 0;
  if (deadline != 0 && !timer_deadline_arm(curr, deadline)) {
    // The bucket is still held, so no waker can have observed the
    // partial wait.
    thread_ptr removed;
    asserts(llrb_futex_remove(bk->waiters, &key, &removed),
            "futex: failed park rollback");
    spinlock_unlock(&bk->lock);
    umem_proc_unlock(p);
    return SYSERR_NOMEM;
  }
  // When the bucket drops, the node, any deadline entry, and PARKED
  // exist together or not at all.
  atomic_store_explicit(&curr->wake_state, FUTEX_PARKED,
                        memory_order_release);
  spinlock_unlock(&bk->lock);
  umem_proc_unlock(p);
  // Unconditional: a wake landing between the bucket drop and the
  // deschedule is carried by thread_unblock_claimed's on_cpu spin.
  uthread_park_blocked();
}

uint64_t futex_sys_requeue(struct thread *curr, uint64_t from, uint64_t to,
                           uint64_t expected, uint64_t count) {
  if (from % 4 != 0 || to % 4 != 0 || from == to) {
    return SYSERR_INVAL;
  }
  struct process *p = curr->proc;
  umem_proc_lock(p);
  if (!user_range_ok(p, from, 4, false) ||
      !user_range_ok(p, to, 4, false)) {
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  struct futex_bucket *bfrom = bucket_of(from);
  struct futex_bucket *bto = bucket_of(to);
  // Both buckets held together, taken in ascending index (one lock when
  // they collide) — the only path in the system to hold two.
  struct futex_bucket *lo = bfrom < bto ? bfrom : bto;
  struct futex_bucket *hi = bfrom < bto ? bto : bfrom;
  spinlock_lock(&lo->lock);
  if (hi != lo) {
    spinlock_lock(&hi->lock);
  }
  // A broadcast racing a state change must not requeue against the new
  // state (Linux's CMP_REQUEUE guard).
  uint32_t word = *(volatile uint32_t *)from;
  if (word != (uint32_t)expected) {
    if (hi != lo) {
      spinlock_unlock(&hi->lock);
    }
    spinlock_unlock(&lo->lock);
    umem_proc_unlock(p);
    return SYSERR_AGAIN;
  }

  uint32_t cap =
      count < FUTEX_REQUEUE_BATCH ? (uint32_t)count : FUTEX_REQUEUE_BATCH;
  uint32_t moved = 0;
  uint64_t next_seq = 0;
  while (moved < cap) {
    llrb_futex_iter iter;
    struct futex_key key;
    thread_ptr t;
    struct futex_key seek = {.address = from, .seq = next_seq};
    llrb_futex_iter_lower_bound(bfrom->waiters, &seek, &iter);
    if (!llrb_futex_iter_next(&iter, &key, &t) || key.address != from) {
      break;
    }
    next_seq = key.seq + 1;
    enum futex_state state = FUTEX_PARKED;
    if (!atomic_compare_exchange_strong_explicit(
            &t->wake_state, &state, FUTEX_MOVING, memory_order_acq_rel,
            memory_order_acquire)) {
      // A CLAIMED waiter's node is left in place for its winner, which
      // is already spinning on one of the buckets we hold.
      continue;
    }
    // The MOVING window is strictly pointer work — a timer IRQ spins on
    // it, so it must never contain an allocation. Extract the node,
    // rewrite the keys, relink, release.
    llrb_futex_node *node;
    thread_ptr extracted;
    asserts(llrb_futex_extract(bfrom->waiters, &key, &extracted, &node) &&
                extracted == t,
            "futex: bucket tree lost moving waiter");
    struct futex_key newkey = {
        .address = to,
        .seq = atomic_fetch_add_explicit(&g_futex_seq, 1,
                                         memory_order_relaxed),
    };
    t->wait_key = newkey;
    asserts(llrb_futex_insert_node(bto->waiters, &newkey, &t, node),
            "futex: requeue relink collided");
    atomic_store_explicit(&t->wake_state, FUTEX_PARKED,
                          memory_order_release);
    moved++;
  }
  if (hi != lo) {
    spinlock_unlock(&hi->lock);
  }
  spinlock_unlock(&lo->lock);
  umem_proc_unlock(p);
  return moved;
}
