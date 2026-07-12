#ifndef gdos_kring_tree_h_INCLUDED
#define gdos_kring_tree_h_INCLUDED

#include <gdos/kring.h>

// Scheme -3: the tree channel (one per process) — child-death events,
// the SIGCHLD slot of the signal decomposition (ipc-process-design.md
// §4). Pure event channel: submits no ops, so any SQE completes with
// SYSERR_NOSYS. Dead children are level state exactly like share edges:
// the zombie sits in the parent's child list until reaped, its
// death-notified bit is the queue entry.

#define KSCHEME_TREE ((int64_t)-3)

#define KEV_CHILD_DEAD KEV(2) // a = dead child pid

#endif // gdos_kring_tree_h_INCLUDED
