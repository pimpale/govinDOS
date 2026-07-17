#ifndef gdos_thread_h_INCLUDED
#define gdos_thread_h_INCLUDED

#include <stdint.h>

// Descriptor passed to SYS_THREAD_SPAWN. The version and exact byte size
// make extension explicit: a kernel never guesses which tail fields exist.
#define GDOS_THREAD_START_VERSION 1u
#define GDOS_THREAD_START_SIZE 56u

// A plain Win64 entry function starts after a conceptual call: one return
// slot followed by the caller-owned 32-byte shadow area. Thread libraries
// reserve this below their allocation's stack top before supplying RSP.
#define GDOS_THREAD_ENTRY_FRAME_BYTES 40u

struct gdos_thread_start {
  uint32_t version;
  uint32_t size;
  uint64_t entry;
  uint64_t argument;
  // Exact initial user RSP. Stack allocation bounds and guard-page policy are
  // entirely userspace state retained by the thread library. A plain C entry
  // normally uses allocation_top - GDOS_THREAD_ENTRY_FRAME_BYTES.
  uint64_t stack_pointer;
  uint64_t fs_base;
  uint64_t gs_base;

  // Optional address of an aligned _Atomic uint32_t in an owned, private,
  // writable event block of the target process. It starts at zero. The
  // kernel release-stores GDOS_THREAD_COMPLETE only after the target is
  // fully descheduled, then performs the block's local doorbell wake.
  // Once an acquire load observes completion, stack/TLS storage is unused.
  // Zero requests no completion notification.
  uint64_t completion_event;
};

static_assert(sizeof(struct gdos_thread_start) == GDOS_THREAD_START_SIZE,
              "gdos thread-start ABI size");

#define GDOS_THREAD_PENDING 0u
#define GDOS_THREAD_COMPLETE 1u

#endif // gdos_thread_h_INCLUDED
