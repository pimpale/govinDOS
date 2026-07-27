#include "slabs.h"

#include "capability.h"
#include "channel.h"
#include "cpu_state.h"
#include "debug.h"
#include "futex.h"
#include "iommu.h"
#include "thread.h"
#include "timer_queue.h"
#include "umem.h"

void slabs_init(void) {
  cpu_state_table_require();
  size_t count = g_cpu_state_table_len;
  asserts(slab_llrb_futex_init(count),
          "slab: futex-tree arena initialization failed");
  asserts(slab_llrb_futex_node_init(count),
          "slab: futex-node arena initialization failed");
  asserts(slab_llrb_grant_id_node_init(count),
          "slab: grant-node arena initialization failed");
  asserts(slab_llrb_pid_process_node_init(count),
          "slab: pid-node arena initialization failed");
  asserts(slab_llrb_tdeadline_node_init(count),
          "slab: deadline-node arena initialization failed");
  asserts(slab_thread_init(count),
          "slab: thread arena initialization failed");
  asserts(slab_process_init(count),
          "slab: process arena initialization failed");
  asserts(slab_ublock_init(count),
          "slab: ublock arena initialization failed");
  asserts(slab_ring_init(count), "slab: ring arena initialization failed");
  asserts(slab_grant_init(count), "slab: grant arena initialization failed");
  asserts(slab_iommu_domain_init(count),
          "slab: IOMMU-domain arena initialization failed");
  asserts(slab_iommu_device_init(count),
          "slab: IOMMU-device arena initialization failed");
  asserts(slab_iommu_mapping_init(count),
          "slab: IOMMU-mapping arena initialization failed");
}
