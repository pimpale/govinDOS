#include "capability.h"

#define LLRB_NAME grant_id
#define LLRB_KEY uint64_t
#define LLRB_VALUE grant_ptr
#define LLRB_COMPARE(a, b) (((*(a)) > (*(b))) - ((*(a)) < (*(b))))
#define LLRB_NODE_MALLOC(size) slab_llrb_grant_id_node_malloc(size)
#define LLRB_NODE_FREE(ptr) slab_llrb_grant_id_node_free(ptr)
#include <llrb/llrb_impl.h>
#undef LLRB_NODE_FREE
#undef LLRB_NODE_MALLOC
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
