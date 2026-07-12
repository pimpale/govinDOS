#ifndef iommu_internal_h_INCLUDED
#define iommu_internal_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

struct acpi_rsdp;

struct iommu_device_id {
  uint16_t segment;
  uint8_t bus;
  uint8_t devfn;
};

struct iommu_hw_domain {
  uint16_t id;
  uint64_t *root;
};

bool vtd_init_required(const struct acpi_rsdp *rsdp);
bool vtd_domain_init(struct iommu_hw_domain *domain);
void vtd_domain_destroy(struct iommu_hw_domain *domain);
bool vtd_covers(struct iommu_device_id id);
bool vtd_attach(struct iommu_hw_domain *domain, struct iommu_device_id id);
bool vtd_detach(struct iommu_hw_domain *domain, struct iommu_device_id id);
bool vtd_map(struct iommu_hw_domain *domain, uint64_t iova, uint64_t phys,
             uint64_t pages, uint32_t permissions);
bool vtd_unmap(struct iommu_hw_domain *domain, uint64_t iova, uint64_t pages);
uint64_t vtd_mapping_leaves(uint64_t iova, uint64_t pages);
void vtd_fault_interrupt(void);

#endif // iommu_internal_h_INCLUDED
