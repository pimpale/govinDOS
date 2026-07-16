#include "capability.h"

#define LLRB_NAME grant_id
#define LLRB_KEY uint64_t
#define LLRB_VALUE grant_ptr
#define LLRB_COMPARE(a, b) (((*(a)) > (*(b))) - ((*(a)) < (*(b))))
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
