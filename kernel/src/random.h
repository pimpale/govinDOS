#ifndef random_h_INCLUDED
#define random_h_INCLUDED

#include <stdint.h>

// Hardware randomness from the arch-specific source. Returns false if the
// CPU has no such source or it stays exhausted after bounded retries.
bool random64(uint64_t *out);

#endif // random_h_INCLUDED
