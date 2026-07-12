// PID registry instantiation. Allocator policy lives here: the defaults are
// the ordinary kernel malloc/free today; a future slab-backed allocator can
// be selected by defining LLRB_MALLOC and LLRB_FREE before the include.

#include "umem.h"

#define LLRB_NAME pid_process
#define LLRB_KEY uint64_t
#define LLRB_VALUE process_ptr
#define LLRB_COMPARE(a, b) (((*(a)) > (*(b))) - ((*(a)) < (*(b))))
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
