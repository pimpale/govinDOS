#ifndef iommu_h_INCLUDED
#define iommu_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

struct acpi_rsdp;
struct process;
struct ring;
struct thread;
struct ksqe;
struct iommu_domain;
struct iommu_device;
struct llrb_domid_map_node;

#define SLAB_NAME iommu_domain
#define SLAB_TYPE struct iommu_domain
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME iommu_device
#define SLAB_TYPE struct iommu_device
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME llrb_domid_map_node
#define SLAB_TYPE struct llrb_domid_map_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

void iommu_init_required(const struct acpi_rsdp *rsdp);
uint64_t iommu_exec(struct thread *curr, struct ring *ring,
                    struct ksqe *sqe);
void iommu_endpoint_destroy(struct ring *ring);
bool iommu_endpoint_destroyable(struct ring *ring);
void iommu_replay(struct ring *ring);
void iommu_report_fault(uint64_t requester, uint64_t iova, uint32_t reason);
uint64_t iommu_enum_maps(struct process *caller, uint64_t base, uint64_t buf,
                         uint64_t cap, uint64_t after);
uint64_t iommu_revoke_map(struct process *caller, uint64_t base,
                          uint64_t domain_id);

#endif // iommu_h_INCLUDED
