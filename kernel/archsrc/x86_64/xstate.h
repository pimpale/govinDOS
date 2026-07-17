#ifndef xstate_h_INCLUDED
#define xstate_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

// Discover the BSP's user-xstate policy and the standard-format XSAVE area
// size it requires. Called once before any user TCB is created.
void x86_xstate_global_init(void);

// Enable that policy on the current CPU. Every CPU must support the BSP's
// selected mask; heterogeneous feature sets fail closed during bring-up.
void x86_xstate_cpu_init(void);

size_t x86_xstate_area_size(void);
uint64_t x86_xstate_mask(void);

// Areas must be x86_xstate_area_size() bytes and 64-byte aligned.
void x86_xstate_area_init(void *area);
void x86_xstate_save(void *area);
void x86_xstate_restore(const void *area);

#endif // xstate_h_INCLUDED
