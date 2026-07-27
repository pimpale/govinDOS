// Fixed-type, sharded slab allocator template. Include with SLAB_NAME and
// SLAB_TYPE defined:
//
//   #define SLAB_NAME thread
//   #define SLAB_TYPE thread
//   #include <slab/slab.h>
//
// This declares one generated singleton allocator for SLAB_TYPE. Include
// slab_impl.h exactly once from a dedicated translation unit to define it.
// SLAB_TYPE may be incomplete here, but must be complete at the implementation
// include. This file intentionally has no header guard.

#ifndef SLAB_NAME
#error "SLAB_NAME must be defined before including slab.h"
#endif
#ifndef SLAB_TYPE
#error "SLAB_TYPE must be defined before including slab.h"
#endif

#include <stdbool.h>
#include <stddef.h>

#define SLAB_DECL_PASTE_(a, b) a##b
#define SLAB_DECL_PASTE(a, b) SLAB_DECL_PASTE_(a, b)
#define SLAB_DECL_T SLAB_DECL_PASTE(slab_, SLAB_NAME)
#define SLAB_DECL_FN(suffix) SLAB_DECL_PASTE(SLAB_DECL_T, suffix)

// Allocate the internal arena table. Must run once, before concurrent use.
// cpu_count is the exclusive upper bound of SLAB_WHICH_CPU().
bool SLAB_DECL_FN(_init)(size_t cpu_count);

// The size argument must equal sizeof(SLAB_TYPE). It is retained so generated
// container allocation hooks can delegate interchangeably to malloc(size) or
// this fixed-type allocator.
SLAB_TYPE *SLAB_DECL_FN(_malloc)(size_t size);
SLAB_TYPE *SLAB_DECL_FN(_zalloc)(size_t size);
void SLAB_DECL_FN(_free)(SLAB_TYPE *obj);

// Opportunistically collect remote frees for the current arena.
void SLAB_DECL_FN(_collect)(void);

// Quiescent diagnostics/teardown. _destroy returns false and changes no
// ownership if any live objects remain; on success it releases every slab and
// the arena table. No operation may race either call.
bool SLAB_DECL_FN(_valid)(void);
bool SLAB_DECL_FN(_destroy)(void);

#undef SLAB_DECL_FN
#undef SLAB_DECL_T
#undef SLAB_DECL_PASTE
#undef SLAB_DECL_PASTE_
