#ifndef iommu_h_INCLUDED
#define iommu_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

struct acpi_rsdp;
struct process;
struct ring;
struct thread;
struct ksqe;

void iommu_init_required(const struct acpi_rsdp *rsdp);
uint64_t iommu_exec(struct thread *curr, struct ring *ring,
                    struct ksqe *sqe);
void iommu_endpoint_destroy(struct ring *ring);
bool iommu_endpoint_destroyable(struct ring *ring);
bool iommu_reap_one_locked(struct process *p);
void iommu_replay(struct ring *ring);
void iommu_report_fault(uint64_t requester, uint64_t iova, uint32_t reason);

#endif // iommu_h_INCLUDED
