#ifndef gdos_block_h_INCLUDED
#define gdos_block_h_INCLUDED

#include <stdatomic.h>
#include <stdint.h>

// Userspace block-service protocol. The client owns and shares one block with
// the server. Only one request may be outstanding on a channel; data lives in
// the remainder of the block and is named by offsets, never pointers.
#define GDOS_BLOCK_MAGIC 0x314b4c424f444747ull // "GGDOBLK1"
#define GDOS_BLOCK_VERSION 1

#define GDOS_BLOCK_INFO  1
#define GDOS_BLOCK_READ  2
#define GDOS_BLOCK_WRITE 3

#define GDOS_BLOCK_STATUS_OK          0
#define GDOS_BLOCK_STATUS_BAD_VERSION 1
#define GDOS_BLOCK_STATUS_BAD_REQUEST 2
#define GDOS_BLOCK_STATUS_RANGE       3
#define GDOS_BLOCK_STATUS_IO          4

struct gdos_block_channel {
  uint64_t magic;
  uint32_t version;
  uint32_t header_bytes;

  // Client publishes request fields then increments request_seq (release).
  // Server publishes response fields then copies it to response_seq (release).
  _Atomic uint32_t request_seq;
  _Atomic uint32_t response_seq;

  uint32_t op;
  uint32_t status;
  uint64_t lba;
  uint32_t block_count;
  uint32_t data_offset;
  uint32_t data_length;
  uint32_t reserved;

  // Returned by INFO and repeated on every completion.
  uint64_t capacity_blocks;
  uint32_t logical_block_size;
  uint32_t max_transfer_blocks;
};

#endif // gdos_block_h_INCLUDED
