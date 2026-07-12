#ifndef kring_h_INCLUDED
#define kring_h_INCLUDED

// Kernel-channel rings, liburing-flavoured: a struct kring caches the
// ring geometry and the two user-owned indices (sq_tail published,
// cq_head consumed) so callers never index the shared block by hand.
// The on-block layout is the ABI's (gdos/kring.h); scheme ids, ops, and
// event types come from the per-scheme gdos/kring_*.h headers.
//
// Shape of a session (cf. liburing):
//
//   struct kring g;
//   kring_create(&g, KSCHEME_IRQ, 4096);
//   struct ksqe *sqe = kring_get_sqe(&g);
//   sqe->op = KIRQ_CLAIM; sqe->a = gsi; sqe->b = cookie;
//   kring_submit(&g);                  // publish sq_tail + doorbell
//   struct kcqe cqe;
//   kring_wait_cqe(&g, &cqe);          // park until one lands, consume it
//   kring_ack(&g);                     // consumption ack: publish cq_head
//                                      // + doorbell, so level state replays
//
// One thread per ring (SPSC is the kernel's rule too). Consumed CQEs
// are invisible to later peeks but only freed for the kernel by the
// next kring_ack — batch consumption, then ack once.

#include <stdint.h>

#include <gdos/kring.h>
#include <gdos/kring_irq.h>

struct kring {
  uint64_t base; // block base — the channel's name to the syscalls
  struct kring_hdr *hdr;
  struct ksqe *sq;
  struct kcqe *cq;
  uint32_t nslots;
  uint32_t sq_tail; // shadow of the user-owned published index
  uint32_t cq_head; // shadow of the user-owned consumed index
};

// Allocate a len-byte block (power-of-two pages) and turn it into a
// kernel channel of `scheme` (KSCHEME_*). Returns 0 and fills *r, or a
// SYSERR_* with nothing allocated.
uint64_t kring_create(struct kring *r, int64_t scheme, uint64_t len);

// Adopt an existing kernel channel at `base` (the header is live: the
// kernel wrote nslots at creation, and the user-owned indices are
// whatever the previous user of the ring published).
void kring_attach(struct kring *r, uint64_t base);

// Free the block (revokes the channel). The ring is dead afterwards.
uint64_t kring_destroy(struct kring *r);

// Next free SQE, or nullptr if the SQ is full against the kernel's
// consumption mirror (submit + re-try, the kernel drains on doorbell).
// Batching: every get_sqe since the last submit is published together.
struct ksqe *kring_get_sqe(struct kring *r);

// Publish the SQEs taken so far and ring the doorbell. The kernel
// drains at most RING_SQ_BATCH per doorbell, so re-rings until its
// sq_head mirror catches up. Returns 0 or the doorbell's SYSERR_*.
uint64_t kring_submit(struct kring *r);

// The next unconsumed CQE, or nullptr if none. The slot stays valid
// until kring_ack: the kernel never posts into unacked slots.
const struct kcqe *kring_peek_cqe(struct kring *r);

// Mark the last-peeked CQE consumed (advances the local head only).
void kring_cqe_seen(struct kring *r);

// Park until a CQE is available, copy it out, and consume it. Returns 0,
// or SYSERR_* from the underlying SYS_BLOCK_WAIT (notably SYSERR_DEAD
// when the block is revoked, SYSERR_EXIST when the side already has a
// parked thread). Does not ack; do that after
// draining what you came for.
uint64_t kring_wait_cqe(struct kring *r, struct kcqe *cqe);

// Consumption ack: publish cq_head and ring the doorbell so the kernel
// replays pending level-state events into the freed slots.
uint64_t kring_ack(struct kring *r);

// IRQ command submission helpers. These return the doorbell result; the
// command's result is the ordinary completion CQE (events may precede it).
uint64_t kring_irq_claim(struct kring *r, uint64_t gsi, uint64_t cookie);
uint64_t kring_irq_release(struct kring *r, uint64_t gsi);
uint64_t kring_irq_ack(struct kring *r, uint64_t gsi, uint64_t seq);
uint64_t kring_irq_msi(struct kring *r, uint64_t child_pid);
uint64_t kring_irq_bind(struct kring *r, uint64_t route_id, uint64_t cookie);

#endif // kring_h_INCLUDED
