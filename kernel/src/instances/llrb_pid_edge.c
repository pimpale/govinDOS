#include "../umem.h"

#include "../cpu_state.h"

#define LLRB_NAME pid_edge
#define LLRB_KEY uint64_t
#define LLRB_VALUE share_edge
#define LLRB_COMPARE(a, b) (((*a) > (*b)) - ((*a) < (*b)))
#define LLRB_TREE_MALLOC(size) malloc(size)
#define LLRB_TREE_FREE(ptr) free(ptr)
#define LLRB_NODE_MALLOC(size) slab_llrb_pid_edge_node_malloc(size)
#define LLRB_NODE_FREE(ptr) slab_llrb_pid_edge_node_free(ptr)
#include <llrb/llrb_impl.h>

#define SLAB_NAME llrb_pid_edge_node
#define SLAB_TYPE llrb_pid_edge_node
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME
