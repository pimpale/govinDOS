// Scheme -1: shares. The per-process channel announcing incoming
// VM_SHAREs (KEV_SHARE). Level state: the share edges themselves — an
// edge with a clear `notified` bit IS the queued event, replayed until
// the CQ has room. Everything here runs under g_umem (posts take the
// ring's stripe inside channel_post).

#include "channel_internal.h"

#include "debug.h"
#include "syscall.h"

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
// fills. This runs at channel creation (backfill) and after every
// doorbell (the consumption ack).
void shares_replay(struct ring *ring) {
  struct process *p = ring->block->owner;
  umem_proc_lock(p);
  for (uint32_t i = 0; i < vec_ublock_ptr_len(p->shared_in); i++) {
    ublock *b;
    vec_ublock_ptr_get(p->shared_in, i, &b);
    share_edge *e = find_edge_to(b, p);
    asserts(e != nullptr, "channel: shared_in block without edge");
    if (!e->notified) {
      if (!channel_post(ring, KEV_SHARE, b->owner->pid, b->base | b->order,
                        0)) {
        break;
      }
      e->notified = true;
    }
  }
  umem_proc_unlock(p);
}

void channel_edge_notify(ublock *b, struct share_edge *e) {
  struct ring *ring = e->to->share_ch;
  if (ring != nullptr &&
      channel_post(ring, KEV_SHARE, b->owner->pid, b->base | b->order, 0)) {
    e->notified = true;
  }
}
