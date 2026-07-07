// Instantiation of the generic vec template for `thread_ptr`. The only
// translation unit that pulls in <vec/vec_impl.h> for this element type,
// so the out-of-line functions exist exactly once.

#include "thread.h"
#define VEC_DTYPE thread_ptr
#include <vec/vec_impl.h>
