#ifndef sha256_h_INCLUDED
#define sha256_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

struct sha256_ctx {
  uint32_t state[8];
  uint64_t bytes;
  uint8_t block[64];
  uint32_t used;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t key_len, const void *data,
                 size_t len, uint8_t out[32]);

#endif
