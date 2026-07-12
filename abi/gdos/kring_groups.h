#ifndef gdos_kring_groups_h_INCLUDED
#define gdos_kring_groups_h_INCLUDED

#include <gdos/kring.h>

// Scheme -2: wait-groups (many per process) — the epoll slot: register
// channel sides, then one park spot hears them all
// (ipc-process-design.md §3). Command channel: ADD/DEL submitted as
// SQEs, readiness and revocation arriving as events. A registered side
// may hold at most one un-consumed KEV_READY; wakes landing meanwhile
// re-arm and re-fire after the consumption ack, so nothing is lost.

#define KSCHEME_GROUPS ((int64_t)-2)

// SQE ops.
#define KGROUP_ADD 1 // a = channel base, b = cookie
#define KGROUP_DEL 2 // a = channel base

// Event CQE types.
#define KEV_READY KEV(3) // a = cookie: the registered side was woken
#define KEV_DEAD KEV(4)  // a = cookie: the registered view was revoked

#endif // gdos_kring_groups_h_INCLUDED
