#ifndef gdos_pci_h_INCLUDED
#define gdos_pci_h_INCLUDED

#include <stdint.h>
#include <stdatomic.h>

#define PCI_DRIVER_START_VERSION 2
#define PCI_DRIVER_MAX_BARS 6

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
  uint32_t irq_route;
  uint64_t service_channel;
};

#define PCI_DRIVER_IOMMU_READY 1
#define PCI_DRIVER_IRQ_GRANTED 2
#define PCI_DRIVER_IRQ_READY   3
#define PCI_DRIVER_LIVE        4
#define PCI_DRIVER_STOP        5
#define PCI_DRIVER_DMA_STOPPED 6

struct pcid_bootstrap {
  uint64_t acpi_rsdp;
  uint64_t driver_image;
  uint64_t driver_image_length;
};

#endif // gdos_pci_h_INCLUDED
