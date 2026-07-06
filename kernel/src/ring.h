#ifndef ring_h_INCLUDED
#define ring_h_INCLUDED

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

struct thread;
struct ring;

// io_uring-style submission/completion rings — the "long syscall" path.
//
// One ring per thread (SPSC by construction: the owning user thread is
// the only producer of SQEs / consumer of CQEs, the ring's worker kthread
// the only consumer / producer). Because the kernel is a single identity-
// mapped address space, the shared area is just a kernel page flagged
// PAGE_U: no pinning, no translation, and the user can poll cq_tail with
// zero syscalls. Syscalls are only needed to sleep (SYS_RING_WAIT) and to
// wake the worker (SYS_RING_ENTER).
//
// Index protocol (io_uring-style monotonic u32 indices, masked on use):
//   sq_tail: user publishes SQEs (release store after filling sq[]).
//   sq_head: worker consumes (release store after copying the SQE out).
//   cq_tail: worker publishes CQEs (release store after filling cq[]).
//   cq_head: user consumes.
// Blocking ops in a submission simply block the worker kthread — that is
// io_uring's async punt reduced to its essence.

#define RING_SLOTS 16 // power of two; sq and cq are the same size

struct ring_sqe {
  uint32_t opcode;
  uint32_t flags;    // unused yet
  uint64_t user_data; // echoed back in the CQE
  uint64_t args[4];
};

struct ring_cqe {
  uint64_t user_data;
  int64_t result;
};

// Fixed layout of the shared page. Offsets are ABI (user code — currently
// raw asm — hardcodes them): header fields at 0..23, sq[] at 64, cq[] at
// 832.
struct ring_shared {
  _Atomic uint32_t sq_head; // 0
  _Atomic uint32_t sq_tail; // 4
  _Atomic uint32_t cq_head; // 8
  _Atomic uint32_t cq_tail; // 12
  uint32_t sq_mask;         // 16 (RING_SLOTS - 1)
  uint32_t cq_mask;         // 20
  uint8_t pad[64 - 24];
  struct ring_sqe sq[RING_SLOTS]; // 64
  struct ring_cqe cq[RING_SLOTS]; // 832
};

static_assert(sizeof(struct ring_sqe) == 48, "SQE ABI");
static_assert(sizeof(struct ring_cqe) == 16, "CQE ABI");
static_assert(offsetof(struct ring_shared, sq) == 64, "ring ABI: sq offset");
static_assert(offsetof(struct ring_shared, cq) == 832, "ring ABI: cq offset");
static_assert(sizeof(struct ring_shared) <= 4096, "ring must fit one page");

// Opcodes.
#define RING_OP_NOP         0 // -> 0
#define RING_OP_DEBUG_WRITE 1 // args[0]=ptr, args[1]=len -> len (validated)
#define RING_OP_DUMMY_READ  2 // -> value from the dummy device (blocks worker)

// Syscall backends (called from syscall.c; `curr` is the calling user
// thread).
uint64_t ring_create(struct thread *curr);          // -> shared base | err
uint64_t ring_enter(struct thread *curr);           // doorbell -> 0 | err

// Teardown (reaper only): stop the worker kthread — waking it if parked,
// waiting out any in-flight op — then free the kernel-side ring struct.
// Does NOT free the shared page; that is an ordinary ublock owned by the
// process, freed by its owner's teardown path.
void ring_destroy(struct ring *r);

// The shared block backing this ring (struct ring is opaque; the reaper
// needs the base to free the ublock after ring_destroy).
struct ring_shared *ring_shared_of(struct ring *r);
// If completions beyond `user_cq_head` already exist (or on error),
// returns the syscall result for the live frame. Otherwise parks the
// calling thread (caller must have saved its frame with rax preloaded
// to 0) until the worker completes something — never returning.
uint64_t ring_wait_user(struct thread *curr, uint32_t user_cq_head);

#endif // ring_h_INCLUDED
