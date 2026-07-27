#include "cpu_state.h"
#include "paging.h"
#include "timer_queue.h"

#define SLAB_NAME llrb_tdeadline_node
#define SLAB_TYPE llrb_tdeadline_node
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME
