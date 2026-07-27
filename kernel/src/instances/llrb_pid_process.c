// PID registry instantiation. The small tree object stays on malloc; its
// fixed-size nodes use the generated per-CPU slab.

#include "umem.h"

#define LLRB_NAME pid_process
#define LLRB_KEY uint64_t
#define LLRB_VALUE process_ptr
#define LLRB_COMPARE(a, b) (((*(a)) > (*(b))) - ((*(a)) < (*(b))))
#define LLRB_NODE_MALLOC(size) slab_llrb_pid_process_node_malloc(size)
#define LLRB_NODE_FREE(ptr) slab_llrb_pid_process_node_free(ptr)
#include <llrb/llrb_impl.h>
#undef LLRB_NODE_FREE
#undef LLRB_NODE_MALLOC
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
