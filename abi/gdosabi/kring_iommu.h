#ifndef gdos_kring_iommu_h_INCLUDED
#define gdos_kring_iommu_h_INCLUDED

#include <stdint.h>

#include <gdosabi/kring.h>

#define KSCHEME_IOMMU ((int64_t)-6)

#define KIOMMU_DOMAIN_CREATE  1
#define KIOMMU_DOMAIN_DESTROY 2
#define KIOMMU_DEVICE_ATTACH  3
#define KIOMMU_DEVICE_DETACH  4
#define KIOMMU_MAP_BLOCK      5
#define KIOMMU_UNMAP_BLOCK    6

// KIOMMU_DEVICE_ATTACH uses a = offset of this request within the IOMMU
// ring block. The capability token itself also lives in that block.
struct kiommu_attach_req {
  uint64_t domain;
  uint64_t token_off;
  uint64_t token_len;
  uint64_t fault_cookie;
};

#define IOMMU_PERM_DEVICE_READ  (1u << 0)
#define IOMMU_PERM_DEVICE_WRITE (1u << 1)

#define IOMMU_PCI_ID(segment, bus, device, function)                         \
  (((uint64_t)(segment) << 16) | ((uint64_t)(bus) << 8) |                   \
   ((uint64_t)(device) << 3) | (uint64_t)(function))

#define KEV_IOMMU_FAULT KEV(6)

#define IOMMU_FAULT_CONTEXT_MISSING 1
#define IOMMU_FAULT_PTE_MISSING     2
#define IOMMU_FAULT_READ_DENIED     3
#define IOMMU_FAULT_WRITE_DENIED    4
#define IOMMU_FAULT_ADDRESS_WIDTH   5
#define IOMMU_FAULT_INTERNAL        6
#define IOMMU_FAULT_ACCESS_WRITE    (1u << 8)

#endif // gdos_kring_iommu_h_INCLUDED
