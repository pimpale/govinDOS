// Scheme -3: tree (the SIGCHLD slot of the signal decomposition). The
// per-process channel announcing dead children (KEV_CHILD_DEAD). Level
// state: the zombies themselves — a dead child sits in p->children
// until destroyed, its death_notified bit is the queue entry. Everything
// here runs under g_umem (posts take the ring-local CQ lock inside
// channel_post).

#include "channel_internal.h"

#include "process.h"
#include "syscall.h"
#include "thread.h"

// Post KEV_CHILD_DEAD for every un-notified dead child, until the CQ
// fills.
void tree_replay(struct ring *ring) {
  struct process *p = ring->block->owner;
  llrb_pid_process_iter iter;
  llrb_pid_process_iter_begin(p->children, &iter);
  struct process *c;
  while (llrb_pid_process_iter_next(&iter, nullptr, &c)) {
    // Only directly dead children notify a live parent. Effective-dead
    // interior descendants belong to the already-announced subtree root.
    if (atomic_load_explicit(&c->dead, memory_order_acquire) &&
        !c->death_notified) {
      if (!channel_post(ring, KEV_CHILD_DEAD, c->pid, c->exit_status, 0)) {
        return;
      }
      c->death_notified = true;
    }
  }
}

void channel_child_dead_notify(struct process *parent, struct process *child) {
  struct ring *ring = parent->tree_ch;
  if (ring != nullptr &&
      channel_post(ring, KEV_CHILD_DEAD, child->pid, child->exit_status, 0)) {
    child->death_notified = true;
  }
}
