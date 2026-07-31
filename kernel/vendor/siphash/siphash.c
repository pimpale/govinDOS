#include "siphash.h"

#define C_ROUNDS 2
#define D_ROUNDS 4

static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

static uint64_t load64_le(const uint8_t *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

static void store64_le(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

struct sip {
  uint64_t v0, v1, v2, v3;
};

static void sipround(struct sip *s) {
  s->v0 += s->v1;
  s->v1 = rotl(s->v1, 13);
  s->v1 ^= s->v0;
  s->v0 = rotl(s->v0, 32);
  s->v2 += s->v3;
  s->v3 = rotl(s->v3, 16);
  s->v3 ^= s->v2;
  s->v0 += s->v3;
  s->v3 = rotl(s->v3, 21);
  s->v3 ^= s->v0;
  s->v2 += s->v1;
  s->v1 = rotl(s->v1, 17);
  s->v1 ^= s->v2;
  s->v2 = rotl(s->v2, 32);
}

static void siphash_core(const void *data, size_t len, const uint8_t key[16],
                         uint8_t *out, size_t outlen) {
  const uint8_t *in = data;
  uint64_t k0 = load64_le(key), k1 = load64_le(key + 8);
  struct sip s = {
      0x736f6d6570736575ull ^ k0,
      0x646f72616e646f6dull ^ k1,
      0x6c7967656e657261ull ^ k0,
      0x7465646279746573ull ^ k1,
  };
  if (outlen == 16) s.v1 ^= 0xee;
  const uint8_t *end = in + (len & ~(size_t)7);
  for (; in != end; in += 8) {
    uint64_t m = load64_le(in);
    s.v3 ^= m;
    for (int i = 0; i < C_ROUNDS; i++) sipround(&s);
    s.v0 ^= m;
  }
  uint64_t b = (uint64_t)len << 56;
  for (size_t i = 0; i < (len & 7); i++) b |= (uint64_t)in[i] << (8 * i);
  s.v3 ^= b;
  for (int i = 0; i < C_ROUNDS; i++) sipround(&s);
  s.v0 ^= b;
  s.v2 ^= outlen == 16 ? 0xee : 0xff;
  for (int i = 0; i < D_ROUNDS; i++) sipround(&s);
  store64_le(out, s.v0 ^ s.v1 ^ s.v2 ^ s.v3);
  if (outlen == 8) return;
  s.v1 ^= 0xdd;
  for (int i = 0; i < D_ROUNDS; i++) sipround(&s);
  store64_le(out + 8, s.v0 ^ s.v1 ^ s.v2 ^ s.v3);
}

uint64_t siphash(const void *data, size_t len, const uint8_t key[16]) {
  uint8_t out[8];
  siphash_core(data, len, key, out, sizeof(out));
  return load64_le(out);
}

void siphash128(const void *data, size_t len, const uint8_t key[16],
                uint8_t out[16]) {
  siphash_core(data, len, key, out, 16);
}
