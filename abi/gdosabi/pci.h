#ifndef gdos_pci_h_INCLUDED
#define gdos_pci_h_INCLUDED

#include <stdint.h>
#include <stdatomic.h>

#include <gdosabi/kring_cap.h>

#define PCI_DRIVER_START_VERSION 4
#define PCI_DRIVER_MAX_BARS 6
#define PCI_DRIVER_MAX_IRQ_ROUTES 32

struct pci_driver_bar {
  uint64_t base;
  uint64_t length;
  uint32_t bar_index;
  uint32_t flags;
};

struct pci_driver_start {
  uint32_t version;
  uint32_t n_bars;
  uint64_t requester_id;
  uint64_t function_id;
  struct pci_driver_bar bars[PCI_DRIVER_MAX_BARS];
  _Atomic uint32_t state;
  uint32_t n_irq_routes;
  struct cap_token iommu_token;
  struct cap_token irq_wildcard;
  struct cap_token irq_routes[PCI_DRIVER_MAX_IRQ_ROUTES];
  uint64_t service_channel;
};

#define PCI_DRIVER_QUEUES_READY 1
#define PCI_DRIVER_LIVE         2
#define PCI_DRIVER_STOP         3
#define PCI_DRIVER_DMA_STOPPED  4

struct pcid_bootstrap {
  uint64_t acpi_rsdp;
  uint64_t driver_image;
  uint64_t driver_image_length;
  uint64_t cap_channel; // zero = pcid creates the default kernel endpoint
  struct cap_token cap_devmem;
  struct cap_token cap_irq;
  struct cap_token cap_iommu;
};

#endif // gdos_pci_h_INCLUDED
