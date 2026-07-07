// Instantiation of the generic vec template for `share_edge`. The only
// translation unit that pulls in <vec/vec_impl.h> for this element type,
// so the out-of-line functions exist exactly once.

#include "umem.h"
#define VEC_DTYPE share_edge
#include <vec/vec_impl.h>
