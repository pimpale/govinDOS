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

// The kernel ring ABI — kring_hdr/ksqe/kcqe layout, scheme ids, KEV_*
// event types, KGROUP_* ops — lives in the shared ABI header
// (abi/gdos/kring.h, the kernel↔userspace contract). This header adds
// the kernel-side endpoint state and entry points.
#include <gdos/kring.h>

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
