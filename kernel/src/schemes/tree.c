// Scheme -3: tree (the SIGCHLD slot of the signal decomposition). The
// per-process channel announcing dead children (KEV_CHILD_DEAD). Level
// state: the zombies themselves — a dead child sits in p->children
// until reaped, its death_notified bit is the queue entry. Everything
// here runs under g_umem (posts take the ring's stripe inside
// channel_post).

#include "channel_internal.h"

#include "process.h"
#include "syscall.h"
#include "thread.h"

// Post KEV_CHILD_DEAD for every un-notified dead child, until the CQ
// fills.
void tree_replay(struct ring *ring) {
  struct process *p = ring->block->owner;
  for (uint32_t i = 0; i < vec_process_ptr_len(p->children); i++) {
    struct process *c;
    vec_process_ptr_get(p->children, i, &c);
    // Only directly dead children notify a live parent. Effective-dead
    // interior descendants belong to the already-announced subtree root.
    if (c->state == PROC_DEAD && !c->death_notified) {
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
