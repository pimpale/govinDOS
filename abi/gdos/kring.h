#ifndef gdos_kring_h_INCLUDED
#define gdos_kring_h_INCLUDED

#include <stdint.h>

// The kernel ring ABI: the layout of a kernel-channel block (a ublock
// VM_SHAREd to a negative scheme id — ipc-process-design.md §2). Shared
// verbatim by the kernel and userspace builds. Between *user* peers a
// block's layout is private convention; this file governs only channels
// whose far end is the kernel.
//
// This header holds only what is common to every scheme: the header, the
// (current) SQE/CQE shapes, and the event-bit convention. Each scheme's
// ops, event types, and scheme id live in their own gdos/kring_*.h — in
// principle a scheme could even define its own entry layouts.
//
// Header at offset 0, then sq[nslots], then cq[nslots], all 32-byte
// entries; nslots = 32 << block order. The kernel keeps authoritative
// indices in its endpoint and never trusts the in-block copies:
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

// Header field byte offsets, for code that addresses the words directly
// (BLOCK_WAIT takes the address of a 32-bit word in the block).
#define KRING_CQ_COUNT_OFF 0
#define KRING_CQ_HEAD_OFF 4
#define KRING_SQ_HEAD_OFF 8
#define KRING_SQ_TAIL_OFF 12

// Event CQE types set the top bit to stay disjoint from SQE ops (which
// completions echo); schemes number their events in the low bits.
#define KEV_EVENT_BIT (1ull << 63)
#define KEV(n) (KEV_EVENT_BIT | (n))

#endif // gdos_kring_h_INCLUDED
