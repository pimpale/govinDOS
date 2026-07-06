#include "ring.h"

#include <stdatomic.h>
#include <stdint.h>

#include "debug.h"
#include "dummydev.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"
#include "umem.h"

// serial-backed character sink used by printf (stdio.c)
extern void putchar_(char c);

// Kernel-private ring state. The lock guards the two sleeper slots and
// the dying handshake; the ring indices themselves are lock-free
// acquire/release.
struct ring {
  struct ring_shared *sh;
  struct process *proc;
  struct spinlock lock;
  struct thread *worker;          // the worker kthread, for teardown
  struct thread *worker_sleeping; // worker parked waiting for SQEs
  struct thread *waiter;          // owning thread parked in ring_wait
  // Set by ring_destroy. Checked by the worker at the top of its loop and
  // again under the lock before sleeping, so a destroy can never slip
  // between "queue is empty" and "park forever".
  _Atomic bool dying;
};

static int64_t ring_execute(struct ring *r, const struct ring_sqe *sqe) {
  switch (sqe->opcode) {
  case RING_OP_NOP:
    return 0;

  case RING_OP_DEBUG_WRITE: {
    uint64_t ptr = sqe->args[0], len = sqe->args[1];
    if (len > 4096) {
      return (int64_t)SYSERR_INVAL;
    }
    if (!user_range_ok(r->proc, ptr, len, false)) {
      return (int64_t)SYSERR_FAULT;
    }
    const char *s = (const char *)ptr;
    for (uint64_t i = 0; i < len; i++) {
      putchar_(s[i]);
    }
    return (int64_t)len;
  }

  case RING_OP_DUMMY_READ:
    // Blocks this worker kthread on its own stack — the calling user
    // thread keeps running (or sleeps in ring_wait) meanwhile.
    return (int64_t)dummydev_read_kthread();

  default:
    return (int64_t)SYSERR_NOSYS;
  }
}

static void ring_complete(struct ring *r, uint64_t user_data, int64_t result) {
  struct ring_shared *sh = r->sh;
  uint32_t tail = atomic_load_explicit(&sh->cq_tail, memory_order_relaxed);
  // CQ full: wait for the consumer. Can't drop (completions are not
  // optional) and can't overwrite; the owning thread must eventually
  // consume since it submitted this much work.
  while (tail - atomic_load_explicit(&sh->cq_head, memory_order_acquire) >=
         RING_SLOTS) {
    yield();
  }
  sh->cq[tail & sh->cq_mask] =
      (struct ring_cqe){.user_data = user_data, .result = result};
  atomic_store_explicit(&sh->cq_tail, tail + 1, memory_order_release);

  spinlock_lock(&r->lock);
  struct thread *waiter = r->waiter;
  r->waiter = nullptr;
  spinlock_unlock(&r->lock);
  if (waiter != nullptr) {
    thread_unblock(waiter);
  }
}

static void ring_worker(void *arg) {
  struct ring *r = arg;
  struct ring_shared *sh = r->sh;
  while (!atomic_load_explicit(&r->dying, memory_order_acquire)) {
    uint32_t head = atomic_load_explicit(&sh->sq_head, memory_order_relaxed);
    if (head == atomic_load_explicit(&sh->sq_tail, memory_order_acquire)) {
      // Empty. Publish sleep intent and re-check under the lock so a
      // doorbell (ring_enter) between check and block can't be lost:
      // enter takes the same lock, so either it sees our slot, or we see
      // its tail update. Same protocol for dying: ring_destroy sets it
      // before taking this lock, so either we see it here or it sees our
      // sleep slot and wakes us.
      spinlock_lock(&r->lock);
      if (atomic_load_explicit(&r->dying, memory_order_acquire)) {
        spinlock_unlock(&r->lock);
        break;
      }
      if (head == atomic_load_explicit(&sh->sq_tail, memory_order_acquire)) {
        r->worker_sleeping = thread_current();
        spinlock_unlock(&r->lock);
        thread_block();
        continue;
      }
      spinlock_unlock(&r->lock);
    }
    // Copy the SQE out before bumping sq_head: after the bump the user
    // may legally reuse the slot.
    struct ring_sqe sqe = sh->sq[head & sh->sq_mask];
    atomic_store_explicit(&sh->sq_head, head + 1, memory_order_release);

    int64_t result = ring_execute(r, &sqe);
    ring_complete(r, sqe.user_data, result);
  }
  // Returning reaps this kthread via thread_enter -> thread_exit.
}

uint64_t ring_create(struct thread *curr) {
  if (curr->ring != nullptr) {
    return SYSERR_EXIST;
  }
  struct ring *r = calloc(1, sizeof(*r));
  if (r == nullptr) {
    return SYSERR_NOMEM;
  }
  // The shared page is an ordinary ublock owned by the process (PAGE_U in
  // its AS; plain kernel memory everywhere else, which is how the worker
  // kthread reaches it from g_as_kernel).
  // TODO: the owner could vm_unmap it out from under the worker; making
  // the ring block owner-unfreeable (or refcounting it) would close that
  // once it matters.
  struct ring_shared *sh =
      umem_alloc(curr->proc, sizeof(struct ring_shared), PAGE_R | PAGE_W);
  if (sh == nullptr) {
    free(r);
    return SYSERR_NOMEM;
  }
  sh->sq_mask = RING_SLOTS - 1;
  sh->cq_mask = RING_SLOTS - 1;
  r->sh = sh;
  r->proc = curr->proc;
  spinlock_init(&r->lock);
  curr->ring = r;
  r->worker = kthread_spawn(ring_worker, r);
  return (uint64_t)sh;
}

struct ring_shared *ring_shared_of(struct ring *r) { return r->sh; }

void ring_destroy(struct ring *r) {
  // Stop the worker before the shared page can be freed: it may be
  // mid-execution (even blocked in a device) with live pointers into sh.
  atomic_store_explicit(&r->dying, true, memory_order_release);
  spinlock_lock(&r->lock);
  struct thread *w = r->worker_sleeping;
  r->worker_sleeping = nullptr;
  spinlock_unlock(&r->lock);
  if (w != nullptr) {
    thread_unblock(w);
  }
  // Wait for the worker to finish any in-flight op and fully deschedule.
  // A worker blocked in a device op is woken by that device eventually,
  // completes the op, and exits at the loop top. Runs in the reaper, so
  // yielding here is fine — and the worker's TCB can't be freed under us:
  // the reaper (us) is the only thing that frees TCBs.
  while (r->worker->status != THREAD_DEAD ||
         atomic_load_explicit(&r->worker->on_cpu, memory_order_acquire)) {
    yield();
  }
  free(r);
}

uint64_t ring_enter(struct thread *curr) {
  struct ring *r = curr->ring;
  if (r == nullptr) {
    return SYSERR_INVAL;
  }
  spinlock_lock(&r->lock);
  struct thread *worker = r->worker_sleeping;
  r->worker_sleeping = nullptr;
  spinlock_unlock(&r->lock);
  if (worker != nullptr) {
    thread_unblock(worker);
  }
  return 0;
}

uint64_t ring_wait_user(struct thread *curr, uint32_t user_cq_head) {
  struct ring *r = curr->ring;
  if (r == nullptr) {
    return SYSERR_INVAL;
  }
  spinlock_lock(&r->lock);
  if (atomic_load_explicit(&r->sh->cq_tail, memory_order_acquire) !=
      user_cq_head) {
    // Completions already pending — no sleep. ring_complete publishes
    // cq_tail before taking the lock, so checking under it cannot miss a
    // wake that happened before we got here.
    spinlock_unlock(&r->lock);
    return 0;
  }
  r->waiter = curr;
  spinlock_unlock(&r->lock);
  uthread_park_blocked();
}
