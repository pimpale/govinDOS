// Instantiation of the generic list template for `thread_ptr`. The only
// translation unit that pulls in <list/list_impl.h> for this element type,
// so the out-of-line functions exist exactly once.

#define LIST_DTYPE thread_ptr
#include "thread.h"
#include <list/list_impl.h>
