#ifndef channel_h_INCLUDED
#define channel_h_INCLUDED

#include <stdint.h>

#include "umem.h"

// Shared-block channels + kernel schemes (ipc-process-design.md §1, §2).
//
// A channel is a shared ublock both sides treat as a ring. The kernel
// contributes exactly two data-plane verbs scoped to the block —
// SYS_BLOCK_DOORBELL (wake the far side / run kernel commands) and
// SYS_BLOCK_WAIT (futex-shaped park on a 32-bit word) — plus consented
// establishment via SYS_VM_SHARE. Between user peers everything inside
// the block is userspace convention; when the far end is a kernel scheme
// (VM_SHARE to a negative id) the layout below is kernel ABI.
//
// There is no session object and no IPC registry: kernel-side state is
// two waiter slots per ublock, a notified bit per share edge, and one
// kchan endpoint per kernel channel — all guarded by the umem lock, all
// torn down by the umem revoke path (channel_block_torn).

// ---------------------------------------------------------------------------
// Kernel ring ABI (kernel channels only)
// ---------------------------------------------------------------------------
//
// Header at offset 0, then sq[nslots], then cq[nslots], all 32-byte
// entries; nslots = 32 << block order. The kernel keeps authoritative
// indices in the kchan endpoint and never trusts the in-block copies:
// cq_count/sq_head are mirrors it writes for the user's convenience, and
// the user-owned sq_tail/cq_head are read once per doorbell/post and
// bounds-checked. All indices are monotonic u32, masked on use.
//
// cq_head (user-owned, CQEs consumed) is what makes level-state replay
// work: the kernel only posts into free slots (cq_count - cq_head <
// nslots), and a doorbell after consuming is the ack that lets pending
// events (unnotified share edges, dead children) replay into the freed
// slots. A lying cq_head only ever starves the liar's own channel.

struct kring_hdr {
  _Atomic uint32_t cq_count; // kernel-owned mirror: CQEs posted
  _Atomic uint32_t cq_head;  // user-owned: CQEs consumed
  _Atomic uint32_t sq_head;  // kernel-owned mirror: SQEs consumed
  _Atomic uint32_t sq_tail;  // user-owned: SQEs published
  uint32_t nslots;           // kernel-written at creation; sq and cq size
  uint8_t pad[64 - 20];
};

struct ksqe {
  uint64_t op;
  uint64_t a, b, c;
};

struct kcqe {
  uint64_t type; // completion: the SQE's op; event: KEV_*
  uint64_t a, b;
  uint64_t status; // 0 or SYSERR_*
};

static_assert(sizeof(struct kring_hdr) == 64, "kring ABI: header size");
static_assert(sizeof(struct ksqe) == 32, "kring ABI: sqe size");
static_assert(sizeof(struct kcqe) == 32, "kring ABI: cqe size");

#define KRING_HDR_SIZE 64
#define KRING_NSLOTS(order) (32u << (order))

// Scheme ids (the negative target space of SYS_VM_SHARE).
#define KSCHEME_SHARES ((int64_t)-1) // one per process; where shares announce
#define KSCHEME_GROUPS ((int64_t)-2) // wait-groups (many per process)
#define KSCHEME_TREE   ((int64_t)-3) // one per process; child-death events

// Event CQE types (top bit set to keep them disjoint from SQE ops).
#define KEV_SHARE      (1ull << 63 | 1) // a = sharer pid, b = base | order
#define KEV_CHILD_DEAD (1ull << 63 | 2) // a = dead child pid
#define KEV_READY      (1ull << 63 | 3) // a = cookie (wait-groups)
#define KEV_DEAD       (1ull << 63 | 4) // a = cookie (wait-groups)

// Wait-group SQE ops (scheme -2).
#define KGROUP_ADD 1 // a = channel base, b = cookie
#define KGROUP_DEL 2 // a = channel base

// A wait-group registration: a channel side's waiter slot, occupied by a
// group instead of a parked thread (registered XOR parked). The two-bit
// lossless dedup: `pending` means an unconsumed KEV_READY is in the
// group's CQ; `armed` means a wake arrived that it doesn't cover yet
// (posted on the next consumption ack). `dead` keeps KEV_DEAD level —
// the registration lingers on the group until the event lands.
typedef struct kreg {
  struct kchan *group;
  struct ublock *b; // registered channel block; nullptr once dead
  bool owner_side;
  uint64_t cookie;
  bool pending;
  uint32_t ev_index; // group-CQ index of the outstanding KEV_READY
  bool armed;
  bool dead; // KEV_DEAD owed (view revoked; auto-removed once posted)
} kreg;
typedef kreg *kreg_ptr;

#define VEC_DTYPE kreg_ptr
#include <vec/vec.h>
#undef VEC_DTYPE

// Kernel-channel endpoint. Lives on the ublock (b->kch); owner-only.
struct kchan {
  int64_t scheme;
  struct ublock *block;
  uint32_t nslots;
  uint32_t sq_head;  // authoritative; header copy is a mirror
  uint32_t cq_count; // authoritative; header copy is a mirror
  // Scheme -2 only: this group's registrations. nregs counts the live
  // ones, bounded so events can never overflow the CQ (2 slots per
  // registration: one outstanding KEV_READY + the final KEV_DEAD).
  vec_kreg_ptr *regs;
  uint32_t nregs;
};

// ---------------------------------------------------------------------------
// Syscall backends (syscall.c). `curr` is the calling user thread; both
// may park it, so the caller must have saved its frame with rax
// preloaded to 0 before calling (the RING_WAIT discipline). A non-parking
// return value overwrites the live frame's rax as usual.
// ---------------------------------------------------------------------------

// SYS_VM_SHARE with a negative target: turn the owned block at `base`
// into a kernel channel of `scheme`. Returns 0 or SYSERR_*.
uint64_t channel_scheme_create(struct process *p, uint64_t base,
                               int64_t scheme);

// SYS_BLOCK_DOORBELL: wake the far side (user channel) or drain + execute
// the SQ in the caller's context (kernel channel). Never parks.
uint64_t channel_block_doorbell(struct thread *curr, uint64_t base);

// SYS_BLOCK_WAIT: park on the 32-bit word at `addr` until a doorbell/CQE
// post/revocation, unless it already differs from `expected`. Parks via
// uthread_park_blocked (never returning) or returns immediately.
uint64_t channel_block_wait(struct thread *curr, uint64_t addr,
                            uint64_t expected);

// ---------------------------------------------------------------------------
// Hooks called by umem.c under the umem lock
// ---------------------------------------------------------------------------

// A share edge b -> target was just created: post KEV_SHARE to the
// target's shares channel if it has one with a free slot (sets the
// edge's notified bit), else leave it clear for replay.
void channel_edge_notify(ublock *b, struct share_edge *e);

// `child` just died: post KEV_CHILD_DEAD to the parent's tree channel if
// it has one with a free slot (sets child->death_notified), else leave
// the bit clear for replay.
void channel_child_dead_notify(struct process *parent, struct process *child);

// The block is being revoked / a share edge torn / the channel identity
// broken (second sharer, ownership move): wake both waiter slots with
// SYSERR_DEAD and, if `destroy_endpoint`, free the kchan. Idempotent.
void channel_block_torn(ublock *b, bool destroy_endpoint);

// Kill-time unhook (process.c): remove every thread of `p` from the
// waiter slots it could be parked in and requeue it for the scheduler's
// dispatch cull.
void channel_unhook_process_locked(struct process *p);

#endif // channel_h_INCLUDED
