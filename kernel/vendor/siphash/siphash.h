#ifndef siphash_h_INCLUDED
#define siphash_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

// SipHash-2-4 (Aumasson & Bernstein), transcribed from the CC0 reference
// implementation at https://github.com/veorq/SipHash. A keyed PRF: with a
// secret key the tag is a MAC; a hash table indexed by it cannot be
// collision-flooded by anyone who lacks the key.

// 64-bit tag, e.g. for hash-table indexing.
uint64_t siphash(const void *data, size_t len, const uint8_t key[16]);

// 128-bit tag, e.g. for authentication tokens.
void siphash128(const void *data, size_t len, const uint8_t key[16],
                uint8_t out[16]);

#endif // siphash_h_INCLUDED
