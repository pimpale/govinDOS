#ifndef hash_h_INCLUDED
#define hash_h_INCLUDED

#include <stdint.h>

// Multiplicative (Fibonacci) hashing: multiply by 2^64 / golden ratio and
// keep the top bits, which the multiply avalanches hardest.
#define HASH_FIB_MULT 0x9E3779B97F4A7C15ull

// Index into a power-of-two table of (1 << table_log2) slots. Callers divide
// the key by its natural granule first (page, futex word, ...): the granule's
// dead low bits carry no entropy, and consecutive granule indices must not
// collide systematically.
static inline uint32_t hash_fib(uint64_t key, unsigned table_log2) {
  return (uint32_t)((key * HASH_FIB_MULT) >> (64 - table_log2));
}

#endif // hash_h_INCLUDED
