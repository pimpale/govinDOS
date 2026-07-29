// Scheme -1: shares. The per-process channel announcing incoming
// VM_SHAREs (KEV_SHARE). Level state: the share edges themselves — an
// edge with a clear `notified` bit IS the queued event, replayed until
// the CQ has room. Everything here runs under g_umem (posts take the
// ring-local CQ lock inside channel_post).

#include "channel_internal.h"

#include "syscall.h"

// Post KEV_SHARE for every un-notified edge pointing at `p`, until the CQ
// fills. This runs at channel creation (backfill) and after every
// doorbell (the consumption ack).
void shares_replay(struct ring *ring) {
  struct process *p = ring->block->owner;
  umem_proc_lock(p);
  while (p->unnotified_head != nullptr) {
    share_edge *e = p->unnotified_head;
    ublock *b = e->block;
    if (!channel_post(ring, KEV_SHARE, b->owner->pid, b->base | b->order,
                      0))
      break;
    umem_edge_pending_unlink_locked(e);
    e->notified = true;
  }
  umem_proc_unlock(p);
}

void channel_edge_notify(ublock *b, struct share_edge *e) {
  struct ring *ring = e->to->share_ch;
  if (ring != nullptr &&
      channel_post(ring, KEV_SHARE, b->owner->pid, b->base | b->order, 0)) {
    umem_edge_pending_unlink_locked(e);
    e->notified = true;
  }
}
