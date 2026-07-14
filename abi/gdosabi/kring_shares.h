#ifndef gdos_kring_shares_h_INCLUDED
#define gdos_kring_shares_h_INCLUDED

#include <gdosabi/kring.h>

// Scheme -1: the shares channel (one per process) — where VM_SHAREs
// pointed at this process announce themselves (ipc-process-design.md
// §2). Pure event channel: submits no ops, so any SQE completes with
// SYSERR_NOSYS. The share edges themselves are the queue (level state);
// un-notified edges replay into the CQ after every consumption ack.

#define KSCHEME_SHARES ((int64_t)-1)

#define KEV_SHARE KEV(1) // a = sharer pid, b = base | order

#endif // gdos_kring_shares_h_INCLUDED
