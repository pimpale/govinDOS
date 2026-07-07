#include "channel.h"

#include <stdatomic.h>
#include <stdint.h>

#include "debug.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"
#include "thread.h"
#include "uaccess.h"

// Everything in this file runs under the umem lock (taken here in the
// syscall backends; already held in the umem.c/process.c hooks). That
// single lock covers the waiter slots, registrations, notified bits, and
// endpoints together with block lifetime, which is what makes the wake
// protocol lossless: a doorbell/post and a park serialize on it, so a
// wake between the parker's last look at its word and its actual park
// cannot be lost.

// ---------------------------------------------------------------------------
// Ring plumbing
// ---------------------------------------------------------------------------

static struct kring_hdr *hdr_of(const struct kchan *kc) {
  return (struct kring_hdr *)kc->block->base;
}

static struct ksqe *sq_of(const struct kchan *kc) {
  return (struct ksqe *)(kc->block->base + KRING_HDR_SIZE);
}

static struct kcqe *cq_of(const struct kchan *kc) {
  return (struct kcqe *)(kc->block->base + KRING_HDR_SIZE +
                         (uint64_t)kc->nslots * sizeof(struct ksqe));
}

static struct thread **side_waiter(ublock *b, bool owner) {
  return owner ? &b->owner_waiter : &b->sharer_waiter;
}

static struct kreg **side_reg(ublock *b, bool owner) {
  return owner ? &b->owner_reg : &b->sharer_reg;
}

// Wake the thread parked in `slot` (if any) with `result` as its syscall
// return value. Safe while holding the umem lock: thread_unblock spins on
// the parker's in-flight context save (which needs nothing we hold — the
// park path drops the umem lock before touching scheduler state) and
// takes a scheduler lock, whose holders never wait on the umem lock
// (as_switch is a bare CR3 write).
static void wake_slot(struct thread **slot, uint64_t result) {
  struct thread *t = *slot;
  if (t == nullptr) {
    return;
  }
  *slot = nullptr;
  thread_deliver_wait_result(t, result);
  thread_unblock(t);
}

static void group_ready(struct kreg *r);

// A wake landing on one side of a block: a parked thread resumes with 0,
// a registration turns into (at most one outstanding) KEV_READY.
static void wake_side(ublock *b, bool owner) {
  if (*side_reg(b, owner) != nullptr) {
    group_ready(*side_reg(b, owner));
  } else {
    wake_slot(side_waiter(b, owner), 0);
  }
}

// Post one CQE. False if the CQ is full — full against the *user-owned*
// consumption index, read once and never trusted: a cq_head run ahead of
// cq_count makes the in-flight count wrap huge and the channel look
// permanently full, starving only the liar. Callers treat a false return
// as "leave the event pending in its level state" (notified bits, dead
// children, armed/dead registrations); completions for consumed SQEs are
// dropped instead, which can only hit a user violating the sizing rules.
static bool kchan_post(struct kchan *kc, uint64_t type, uint64_t a,
                       uint64_t b, uint64_t status) {
  struct kring_hdr *h = hdr_of(kc);
  uint32_t consumed = atomic_load_explicit(&h->cq_head, memory_order_acquire);
  if (kc->cq_count - consumed >= kc->nslots) {
    return false;
  }
  cq_of(kc)[kc->cq_count % kc->nslots] =
      (struct kcqe){.type = type, .a = a, .b = b, .status = status};
  kc->cq_count++;
  atomic_store_explicit(&h->cq_count, kc->cq_count, memory_order_release);
  // The post is itself a wake on the channel's owner side — a parked
  // owner resumes, a registered kernel channel forwards into its group.
  // Bounded recursion: a group cannot be registered in a group.
  wake_side(kc->block, true);
  return true;
}

// ---------------------------------------------------------------------------
// Scheme -1: shares
// ---------------------------------------------------------------------------

static share_edge *find_edge_to(ublock *b, const struct process *p) {
  for (uint32_t i = 0; i < vec_share_edge_len(b->sharers); i++) {
    share_edge *e = vec_share_edge_at(b->sharers, i);
    if (e->to == p) {
      return e;
    }
  }
  return nullptr;
}

// Post KEV_SHARE for every un-notified edge pointing at `p`, until the CQ
// fills. The edges *are* the queue (level state); this runs at channel
// creation (backfill) and after every doorbell (the consumption ack).
static void shares_replay(struct process *p) {
  struct kchan *kc = p->share_ch;
  if (kc == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < vec_ublock_ptr_len(p->shared_in); i++) {
    ublock *b;
    vec_ublock_ptr_get(p->shared_in, i, &b);
    share_edge *e = find_edge_to(b, p);
    asserts(e != nullptr, "channel: shared_in block without edge");
    if (!e->notified) {
      if (!kchan_post(kc, KEV_SHARE, b->owner->pid, b->base | b->order, 0)) {
        return;
      }
      e->notified = true;
    }
  }
}

void channel_edge_notify(ublock *b, struct share_edge *e) {
  struct kchan *kc = e->to->share_ch;
  if (kc != nullptr &&
      kchan_post(kc, KEV_SHARE, b->owner->pid, b->base | b->order, 0)) {
    e->notified = true;
  }
}

// ---------------------------------------------------------------------------
// Scheme -3: tree (the SIGCHLD slot of the signal decomposition)
// ---------------------------------------------------------------------------

// Post KEV_CHILD_DEAD for every un-notified dead child, until the CQ
// fills. Dead children are level state exactly like share edges: the
// zombie sits in p->children until reaped, its death_notified bit is the
// queue entry.
static void tree_replay(struct process *p) {
  struct kchan *kc = p->tree_ch;
  if (kc == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < vec_process_ptr_len(p->children); i++) {
    struct process *c;
    vec_process_ptr_get(p->children, i, &c);
    if (c->state == PROC_DEAD && !c->death_notified) {
      if (!kchan_post(kc, KEV_CHILD_DEAD, c->pid, 0, 0)) {
        return;
      }
      c->death_notified = true;
    }
  }
}

void channel_child_dead_notify(struct process *parent, struct process *child) {
  struct kchan *kc = parent->tree_ch;
  if (kc != nullptr && kchan_post(kc, KEV_CHILD_DEAD, child->pid, 0, 0)) {
    child->death_notified = true;
  }
}

// ---------------------------------------------------------------------------
// Scheme -2: wait-groups
// ---------------------------------------------------------------------------

// A wake on a registered side. Two bits make this lossless: with no
// KEV_READY outstanding, post one (or arm if the CQ is full); with one
// outstanding, arm — the unconsumed event covers this wake, and the
// arming re-fires it after the consumption ack, so a wake landing
// between a drain and its ack never vanishes.
static void group_ready(struct kreg *r) {
  if (r->pending) {
    r->armed = true;
    return;
  }
  if (kchan_post(r->group, KEV_READY, r->cookie, 0, 0)) {
    r->pending = true;
    r->ev_index = r->group->cq_count - 1;
  } else {
    r->armed = true;
  }
}

static void reg_free(struct kreg *r) {
  vec_kreg_ptr *v = r->group->regs;
  for (uint32_t i = 0; i < vec_kreg_ptr_len(v); i++) {
    struct kreg *q;
    vec_kreg_ptr_get(v, i, &q);
    if (q == r) {
      vec_kreg_ptr_swap_and_pop(v, i);
      free(r);
      return;
    }
  }
  asserts(false, "channel: registration missing from its group");
}

// The registered view was revoked: the registration owes its group one
// KEV_DEAD (POLLHUP) and is auto-removed once it lands. The block
// pointer is dead from here on.
static void reg_died(struct kreg *r) {
  r->b = nullptr;
  r->dead = true;
  r->group->nregs--;
  if (kchan_post(r->group, KEV_DEAD, r->cookie, 0, 0)) {
    reg_free(r);
  }
  // else: it stays on the group, flagged, replayed on the next ack.
}

// Consumption ack + level-state replay for a group: retire KEV_READYs
// the user has consumed (their slots are below cq_head), then post
// whatever the level state owes — KEV_DEAD for dead registrations,
// re-fired KEV_READY for armed ones.
static void group_replay(struct kchan *kc) {
  uint32_t consumed =
      atomic_load_explicit(&hdr_of(kc)->cq_head, memory_order_acquire);
  // Backwards: posting a dead reg's event removes it from the vec.
  for (uint32_t i = vec_kreg_ptr_len(kc->regs); i > 0; i--) {
    struct kreg *r;
    vec_kreg_ptr_get(kc->regs, i - 1, &r);
    if (r->pending && (int32_t)(consumed - r->ev_index) > 0) {
      r->pending = false;
    }
    if (r->dead) {
      if (kchan_post(kc, KEV_DEAD, r->cookie, 0, 0)) {
        reg_free(r);
      }
    } else if (r->armed && !r->pending) {
      if (kchan_post(kc, KEV_READY, r->cookie, 0, 0)) {
        r->armed = false;
        r->pending = true;
        r->ev_index = kc->cq_count - 1;
      }
    }
  }
}

// Resolve `base` to (block, side) for the calling process, applying the
// channel rules: a kernel channel has only its owner side; a user
// channel needs exactly one sharer and the caller a participant.
// Returns nullptr on any rule violation.
static ublock *resolve_side(struct process *p, uint64_t base, bool *owner_out) {
  ublock *b = umem_view_locked(p, base, 1);
  if (b == nullptr) {
    return nullptr;
  }
  if (b->kch != nullptr) {
    *owner_out = true; // kchan blocks have no sharers: p is the owner
    return b;
  }
  if (vec_share_edge_len(b->sharers) != 1) {
    return nullptr;
  }
  bool is_owner = b->owner == p;
  if (!is_owner && vec_share_edge_at(b->sharers, 0)->to != p) {
    return nullptr;
  }
  *owner_out = is_owner;
  return b;
}

static uint64_t group_add(struct thread *curr, struct kchan *group,
                          uint64_t base, uint64_t cookie) {
  bool owner;
  ublock *b = resolve_side(curr->proc, base, &owner);
  if (b == nullptr) {
    return SYSERR_INVAL;
  }
  if (b->kch != nullptr && b->kch->scheme == KSCHEME_GROUPS) {
    return SYSERR_INVAL; // group-in-group
  }
  if (b->kch == nullptr) {
    // Registering a dead peer's channel is refused the same way parking
    // on it is.
    struct process *peer =
        owner ? vec_share_edge_at(b->sharers, 0)->to : b->owner;
    if (peer->state == PROC_DEAD) {
      return SYSERR_DEAD;
    }
  }
  if (*side_waiter(b, owner) != nullptr || *side_reg(b, owner) != nullptr) {
    return SYSERR_EXIST; // registered XOR parked
  }
  if (2 * (group->nregs + 1) > group->nslots) {
    return SYSERR_NOMEM; // events could overflow the CQ
  }

  struct kreg *r = calloc(1, sizeof(*r));
  asserts(r != nullptr, "channel: kreg alloc failed");
  r->group = group;
  r->b = b;
  r->owner_side = owner;
  r->cookie = cookie;
  *side_reg(b, owner) = r;
  vec_kreg_ptr_push(group->regs, &r);
  group->nregs++;

  // Kernel channels get backfill for free: the kernel can see its own
  // queue, so a non-empty CQ is announced immediately. (User channels
  // follow the ordering discipline instead: register before acking the
  // peer and the channel is provably cold here.)
  if (b->kch != nullptr) {
    uint32_t consumed = atomic_load_explicit(&hdr_of(b->kch)->cq_head,
                                             memory_order_acquire);
    if (b->kch->cq_count != consumed) {
      group_ready(r);
    }
  }
  return 0;
}

static uint64_t group_del(struct thread *curr, struct kchan *group,
                          uint64_t base) {
  bool owner;
  ublock *b = resolve_side(curr->proc, base, &owner);
  if (b == nullptr) {
    return SYSERR_INVAL;
  }
  struct kreg *r = *side_reg(b, owner);
  if (r == nullptr || r->group != group) {
    return SYSERR_INVAL;
  }
  // Already-posted events stay; tolerating stale cookies is userspace's
  // problem, exactly as with epoll. The side is parkable again.
  *side_reg(b, owner) = nullptr;
  group->nregs--;
  reg_free(r);
  return 0;
}

// ---------------------------------------------------------------------------
// Kernel-channel command execution (borrowed context: the doorbeller's)
// ---------------------------------------------------------------------------

static uint64_t scheme_exec(struct thread *curr, struct kchan *kc,
                            const struct ksqe *sqe) {
  switch (kc->scheme) {
  case KSCHEME_GROUPS:
    switch (sqe->op) {
    case KGROUP_ADD:
      return group_add(curr, kc, sqe->a, sqe->b);
    case KGROUP_DEL:
      return group_del(curr, kc, sqe->a);
    default:
      return SYSERR_NOSYS;
    }
  default:
    // Pure event channels take no commands; a submitted SQE still
    // consumes its slot and completes, carrying the error.
    return SYSERR_NOSYS;
  }
}

static void kchan_run(struct thread *curr, struct kchan *kc) {
  struct kring_hdr *h = hdr_of(kc);
  uint32_t tail = atomic_load_explicit(&h->sq_tail, memory_order_acquire);
  if (tail - kc->sq_head <= kc->nslots) {
    while (kc->sq_head != tail) {
      // Copy the SQE out before publishing consumption: after the mirror
      // store the user may legally reuse the slot. The whole block is
      // hostile; only this copy is validated.
      struct ksqe sqe = sq_of(kc)[kc->sq_head % kc->nslots];
      kc->sq_head++;
      atomic_store_explicit(&h->sq_head, kc->sq_head, memory_order_release);
      uint64_t status = scheme_exec(curr, kc, &sqe);
      kchan_post(kc, sqe.op, sqe.a, sqe.b, status);
    }
  }
  // else: lying sq_tail — the doorbell completes nothing. Either way the
  // doorbell doubles as the consumption ack: the user advanced cq_head
  // before ringing, so pending level-state events can replay now.
  switch (kc->scheme) {
  case KSCHEME_SHARES:
    shares_replay(kc->block->owner);
    break;
  case KSCHEME_TREE:
    tree_replay(kc->block->owner);
    break;
  case KSCHEME_GROUPS:
    group_replay(kc);
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Syscall backends
// ---------------------------------------------------------------------------

uint64_t channel_scheme_create(struct process *p, uint64_t base,
                               int64_t scheme) {
  umem_lock();
  ublock *b = umem_owned_locked(p, base);
  if (b == nullptr || b->kch != nullptr ||
      vec_share_edge_len(b->sharers) != 0) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  struct kchan **anchor = nullptr; // schemes that are one-per-process
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

  struct kchan *kc = calloc(1, sizeof(*kc));
  asserts(kc != nullptr, "channel: kchan alloc failed");
  kc->scheme = scheme;
  kc->block = b;
  kc->nslots = KRING_NSLOTS(b->order);
  b->kch = kc;

  // The kernel is the trusted producer: it owns the header from here on.
  struct kring_hdr *h = hdr_of(kc);
  memset(h, 0, sizeof(*h));
  h->nslots = kc->nslots;

  // Level state that predates the channel announces itself now: share
  // edges with clear notified bits, children already dead.
  switch (scheme) {
  case KSCHEME_SHARES:
    *anchor = kc;
    shares_replay(p);
    break;
  case KSCHEME_TREE:
    *anchor = kc;
    tree_replay(p);
    break;
  case KSCHEME_GROUPS:
    vec_kreg_ptr_new(&kc->regs);
    break;
  default:
    break;
  }
  umem_unlock();
  return 0;
}

uint64_t channel_block_doorbell(struct thread *curr, uint64_t base) {
  struct process *p = curr->proc;
  umem_lock();
  bool owner;
  ublock *b = resolve_side(p, base, &owner);
  if (b == nullptr) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  if (b->kch != nullptr) {
    if (b->owner != p) {
      umem_unlock();
      return SYSERR_INVAL;
    }
    kchan_run(curr, b->kch);
    umem_unlock();
    return 0;
  }
  // User channel: wake whoever is on the far side. Never reads the block.
  wake_side(b, !owner);
  umem_unlock();
  return 0;
}

uint64_t channel_block_wait(struct thread *curr, uint64_t addr,
                            uint64_t expected) {
  struct process *p = curr->proc;
  if (addr % 4 != 0) {
    return SYSERR_INVAL;
  }
  umem_lock();
  // A kill can land between the dispatcher's death check and here; a
  // dead process must not park (nobody would cull it until reap).
  if (p->state == PROC_DEAD) {
    umem_unlock();
    uthread_park_exit();
  }
  bool owner;
  ublock *b = resolve_side(p, addr, &owner);
  // The caller's own view must be readable (it could have guarded the
  // page with vm_protect); the kernel is about to load through it.
  if (b == nullptr || !user_range_ok(p, addr, 4, false)) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  if (b->kch == nullptr) {
    // Fail fast if the peer process is no longer live: only threads
    // already parked at its death wait for reap-time revocation.
    struct process *peer =
        owner ? vec_share_edge_at(b->sharers, 0)->to : b->owner;
    if (peer->state == PROC_DEAD) {
      umem_unlock();
      return SYSERR_DEAD;
    }
  }
  if (*side_waiter(b, owner) != nullptr || *side_reg(b, owner) != nullptr) {
    umem_unlock();
    return SYSERR_EXIST; // registered XOR parked; one thread per side
  }

  // The word is untrusted: only compared, never interpreted. A lying peer
  // can only misdirect waiters who chose to rendezvous with it.
  uint32_t word = *(volatile uint32_t *)addr;
  if (word != (uint32_t)expected) {
    umem_unlock();
    return 0;
  }
  *side_waiter(b, owner) = curr;
  umem_unlock();
  uthread_park_blocked();
}

// ---------------------------------------------------------------------------
// Revoke-path hooks (umem.c / process.c, under the umem lock)
// ---------------------------------------------------------------------------

void channel_block_torn(ublock *b, bool destroy_endpoint) {
  wake_slot(&b->owner_waiter, SYSERR_DEAD);
  wake_slot(&b->sharer_waiter, SYSERR_DEAD);
  if (b->owner_reg != nullptr) {
    struct kreg *r = b->owner_reg;
    b->owner_reg = nullptr;
    reg_died(r);
  }
  if (b->sharer_reg != nullptr) {
    struct kreg *r = b->sharer_reg;
    b->sharer_reg = nullptr;
    reg_died(r);
  }
  if (destroy_endpoint && b->kch != nullptr) {
    struct kchan *kc = b->kch;
    if (kc->scheme == KSCHEME_SHARES) {
      asserts(b->owner->share_ch == kc, "channel: share_ch mismatch");
      b->owner->share_ch = nullptr;
    } else if (kc->scheme == KSCHEME_TREE) {
      asserts(b->owner->tree_ch == kc, "channel: tree_ch mismatch");
      b->owner->tree_ch = nullptr;
    } else if (kc->scheme == KSCHEME_GROUPS) {
      // The group dies with its block: detach every registration (live
      // ones become plain parkable sides again; nobody is told — the
      // listener itself is gone).
      while (vec_kreg_ptr_len(kc->regs) > 0) {
        struct kreg *r;
        vec_kreg_ptr_get(kc->regs, 0, &r);
        vec_kreg_ptr_swap_and_pop(kc->regs, 0);
        if (r->b != nullptr) {
          *side_reg(r->b, r->owner_side) = nullptr;
        }
        free(r);
      }
      vec_kreg_ptr_delete(&kc->regs);
    }
    free(kc);
    b->kch = nullptr;
  }
}

void channel_unhook_process_locked(struct process *p) {
  // A thread can only be parked in a waiter slot of a block its process
  // has a view of, so p's blocks + shared_in cover every park site of
  // p's threads. Unhook and requeue them; the scheduler culls them at
  // dispatch (their process is dead by the time this runs).
  for (uint32_t i = 0; i < vec_ublock_ptr_len(p->blocks); i++) {
    ublock *b;
    vec_ublock_ptr_get(p->blocks, i, &b);
    if (b->owner_waiter != nullptr && b->owner_waiter->proc == p) {
      struct thread *t = b->owner_waiter;
      b->owner_waiter = nullptr;
      thread_unblock(t);
    }
  }
  for (uint32_t i = 0; i < vec_ublock_ptr_len(p->shared_in); i++) {
    ublock *b;
    vec_ublock_ptr_get(p->shared_in, i, &b);
    if (b->sharer_waiter != nullptr && b->sharer_waiter->proc == p) {
      struct thread *t = b->sharer_waiter;
      b->sharer_waiter = nullptr;
      thread_unblock(t);
    }
  }
}
