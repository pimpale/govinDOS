#ifndef allocator_h_INCLUDED
#define allocator_h_INCLUDED

#include <stdint.h>

struct allocator_entry


struct allocator {
    // how many levels there are
    uint32_t levels;
    // heap, has levels^2-1 entries
    uint8_t* heap;
};

#endif // allocator_h_INCLUDED
