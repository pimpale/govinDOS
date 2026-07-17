// PCI configuration/lifecycle manager. Init delegates the hardware roots;
// pcid narrows them to the firmware, ECAM, BAR, requester, and IRQ authority
// needed by each operation or driver.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gdosabi/block.h>
#include <gdosabi/kring_iommu.h>
#include <gdosabi/pci.h>
#include <gdosabi/kring_tree.h>
#include <gdosabi/syscall.h>

#include <kring.h>
#include <pe.h>
#include <string.h>
#include <sys.h>

#define PAGE_SIZE 4096ull
#define MAX_FW_PAGES 128
#define MAX_MCFG_ALLOCS 8
#define MAX_FUNCTIONS 256

#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_CLASS_REV 0x08
#define PCI_HEADER_TYPE 0x0E
#define PCI_CAP_PTR 0x34
#define PCI_BAR0 0x10
#define PCI_COMMAND_IO 0x1
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4
#define PCI_COMMAND_INT_DISABLE 0x400
#define PCI_STATUS_CAP_LIST 0x10

struct [[gnu::packed]] acpi_rsdp {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
};

struct [[gnu::packed]] acpi_header {
  char signature[4];
  uint32_t length;
  uint8_t revision, checksum;
  char oem_id[6], oem_table_id[8];
  uint32_t oem_revision, creator_id, creator_revision;
};

struct [[gnu::packed]] acpi_mcfg {
  struct acpi_header header;
  uint64_t reserved;
};

struct [[gnu::packed]] mcfg_allocation {
  uint64_t base;
  uint16_t segment;
  uint8_t start_bus, end_bus;
  uint32_t reserved;
};

struct pci_bar {
  uint64_t base, size;
  uint8_t index;
  bool memory, is64, prefetch;
};

struct pci_function {
  uint64_t requester_id;
  volatile uint8_t *config;
  uint16_t vendor, device;
  uint8_t class_code, subclass, prog_if, revision, header_type;
  uint8_t parent_bus;
  uint8_t msi_cap, msix_cap, pcie_cap, pm_cap;
  struct pci_bar bars[6];
  uint8_t n_bars;
};

static uint64_t g_fw_pages[MAX_FW_PAGES];
static uint32_t g_nfw_pages;
static struct mcfg_allocation g_allocs[MAX_MCFG_ALLOCS];
static uint32_t g_nallocs;
static struct pci_function g_functions[MAX_FUNCTIONS];
static uint32_t g_nfunctions;
static const struct pcid_bootstrap *g_bootstrap;
static struct kring g_tree;
static struct kring g_irq_control;
static struct kring g_cap_control;

struct managed_driver {
  struct pci_function *function;
  struct pci_driver_start *setup;
  uint64_t pid;
  uint64_t bar_block;
  uint64_t table_block;
  uint64_t service_block;
  volatile uint32_t *msix_entry;
  uint8_t interrupt_cap;
  bool use_msix;
};
static struct managed_driver g_driver;
static uint64_t g_driver_generation;

static uint64_t page_floor(uint64_t v) { return v & ~(PAGE_SIZE - 1); }
static uint64_t page_ceil(uint64_t v) {
  return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint64_t map_device(uint64_t base, uint64_t length, uint32_t flags) {
  struct cap_token token;
  uint64_t rc = kring_cap_subgrant(&g_cap_control, &g_bootstrap->cap_devmem,
      base, length, flags, KCAP_PARAM_P0 | KCAP_PARAM_P1 | KCAP_PARAM_P2,
      &token);
  return rc == 0 ? sys_vm_map_device(&token, flags) : rc;
}

static bool checksum_zero(const void *ptr, uint64_t len) {
  const uint8_t *p = ptr;
  uint8_t sum = 0;
  for (uint64_t i = 0; i < len; i++)
    sum += p[i];
  return sum == 0;
}

static bool sig(const char *a, const char *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return false;
  return true;
}

static bool firmware_page_mapped(uint64_t page) {
  for (uint32_t i = 0; i < g_nfw_pages; i++)
    if (g_fw_pages[i] == page)
      return true;
  return false;
}

static bool map_firmware(uint64_t base, uint64_t length) {
  if (length == 0 || base + length < base)
    return false;
  uint64_t first = page_floor(base), end = page_ceil(base + length);
  for (uint64_t page = first; page < end; page += PAGE_SIZE) {
    if (firmware_page_mapped(page))
      continue;
    if (g_nfw_pages == MAX_FW_PAGES ||
        map_device(page, PAGE_SIZE,
                   VM_DEVICE_READ | VM_DEVICE_FIRMWARE) != 0)
      return false;
    g_fw_pages[g_nfw_pages++] = page;
  }
  return true;
}

static const struct acpi_header *map_sdt(uint64_t address) {
  if (!map_firmware(address, sizeof(struct acpi_header)))
    return nullptr;
  const struct acpi_header *h = (const void *)address;
  if (h->length < sizeof(*h) || h->length > (1u << 20) ||
      !map_firmware(address, h->length) || !checksum_zero(h, h->length))
    return nullptr;
  return h;
}

static const struct acpi_header *find_table(const struct acpi_rsdp *rsdp,
                                            const char signature[4]) {
  bool xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
  uint64_t root_addr = xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;
  const struct acpi_header *root = map_sdt(root_addr);
  if (root == nullptr || !sig(root->signature, xsdt ? "XSDT" : "RSDT", 4))
    return nullptr;
  uint32_t entry_size = xsdt ? 8 : 4;
  if ((root->length - sizeof(*root)) % entry_size)
    return nullptr;
  const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
  uint32_t count = (root->length - sizeof(*root)) / entry_size;
  for (uint32_t i = 0; i < count; i++) {
    uint64_t address = 0;
    memcpy(&address, entries + i * entry_size, entry_size);
    const struct acpi_header *h = map_sdt(address);
    if (h != nullptr && sig(h->signature, signature, 4))
      return h;
  }
  return nullptr;
}

static uint16_t cfg16(volatile uint8_t *c, uint32_t off) {
  return *(volatile uint16_t *)(c + off);
}
static uint32_t cfg32(volatile uint8_t *c, uint32_t off) {
  return *(volatile uint32_t *)(c + off);
}
static void cfg16_write(volatile uint8_t *c, uint32_t off, uint16_t value) {
  *(volatile uint16_t *)(c + off) = value;
}
static void cfg32_write(volatile uint8_t *c, uint32_t off, uint32_t value) {
  *(volatile uint32_t *)(c + off) = value;
}

static bool power_of_two(uint64_t v) { return v != 0 && !(v & (v - 1)); }

static void signal_state(struct pci_driver_start *setup, uint32_t state) {
  atomic_store_explicit(&setup->state, state, memory_order_release);
  sys_block_doorbell((uint64_t)setup);
}

static bool wait_state(struct pci_driver_start *setup, uint32_t wanted) {
  for (;;) {
    uint32_t state =
        atomic_load_explicit(&setup->state, memory_order_acquire);
    if (state >= wanted)
      return true;
    if (sys_block_wait(&setup->state, state) != 0)
      return false;
  }
}

static void probe_bars(struct pci_function *f) {
  volatile uint8_t *c = f->config;
  uint16_t command = cfg16(c, PCI_COMMAND);
  cfg16_write(c, PCI_COMMAND,
              (command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                           PCI_COMMAND_MASTER)) |
                  PCI_COMMAND_INT_DISABLE);
  (void)cfg16(c, PCI_COMMAND); // posted config-write drain

  uint8_t max = (f->header_type & 0x7f) == 1 ? 2 : 6;
  for (uint8_t i = 0; i < max; i++) {
    uint32_t off = PCI_BAR0 + i * 4;
    uint32_t lo = cfg32(c, off);
    if (lo == 0 || lo == 0xffffffff)
      continue;
    struct pci_bar bar = {.index = i};
    if (lo & 1) {
      continue; // I/O BARs are unsupported in v1.
    }
    uint32_t type = (lo >> 1) & 3;
    if (type != 0 && type != 2)
      continue;
    bar.memory = true;
    bar.is64 = type == 2;
    bar.prefetch = (lo & 8) != 0;
    uint32_t hi = 0;
    if (bar.is64) {
      if (i + 1 >= max)
        continue;
      hi = cfg32(c, off + 4);
    }

    cfg32_write(c, off, 0xffffffffu);
    if (bar.is64)
      cfg32_write(c, off + 4, 0xffffffffu);
    uint32_t mask_lo = cfg32(c, off);
    uint32_t mask_hi = bar.is64 ? cfg32(c, off + 4) : 0;
    if (bar.is64)
      cfg32_write(c, off + 4, hi);
    cfg32_write(c, off, lo);

    uint64_t mask = ((uint64_t)mask_hi << 32) | (mask_lo & ~0xfull);
    bar.base = ((uint64_t)hi << 32) | (lo & ~0xfull);
    bar.size = ~mask + 1;
    if (!power_of_two(bar.size) || bar.base == 0 ||
        (bar.base & (bar.size - 1)) || bar.base + bar.size < bar.base)
      continue;
    f->bars[f->n_bars++] = bar;
    if (bar.is64)
      i++;
  }
  // Discovery never enables a function; preserve other command bits but
  // deliberately leave decoding, BME, and INTx disabled.
  cfg16_write(c, PCI_COMMAND,
              (command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                           PCI_COMMAND_MASTER)) |
                  PCI_COMMAND_INT_DISABLE);
}

static bool walk_caps(struct pci_function *f) {
  if (!(cfg16(f->config, PCI_STATUS) & PCI_STATUS_CAP_LIST))
    return true;
  uint64_t visited[4] = {0};
  uint8_t ptr = *(volatile uint8_t *)(f->config + PCI_CAP_PTR) & ~3u;
  for (uint32_t steps = 0; ptr != 0; steps++) {
    if (steps == 48 || ptr < 0x40 || ptr > 0xfc || (ptr & 3))
      return false;
    uint8_t slot = ptr >> 2;
    uint64_t bit = 1ull << (slot & 63);
    if (visited[slot >> 6] & bit)
      return false;
    visited[slot >> 6] |= bit;
    uint8_t id = *(volatile uint8_t *)(f->config + ptr);
    if (id == 0x05)
      f->msi_cap = ptr;
    else if (id == 0x11)
      f->msix_cap = ptr;
    else if (id == 0x10)
      f->pcie_cap = ptr;
    else if (id == 0x01)
      f->pm_cap = ptr;
    ptr = *(volatile uint8_t *)(f->config + ptr + 1) & ~3u;
  }
  return true;
}

static struct pci_function *record_function(struct mcfg_allocation *a,
                                             uint8_t bus, uint8_t dev,
                                             uint8_t fn, uint8_t parent) {
  if (g_nfunctions == MAX_FUNCTIONS)
    return nullptr;
  uint64_t offset = ((uint64_t)(bus - a->start_bus) << 20) |
                    ((uint64_t)dev << 15) | ((uint64_t)fn << 12);
  volatile uint8_t *c = (volatile uint8_t *)(a->base + offset);
  uint16_t vendor = cfg16(c, 0);
  if (vendor == 0xffff)
    return nullptr;
  struct pci_function *f = &g_functions[g_nfunctions++];
  memset(f, 0, sizeof(*f));
  uint32_t id = cfg32(c, 0), cr = cfg32(c, PCI_CLASS_REV);
  f->config = c;
  f->vendor = id;
  f->device = id >> 16;
  f->revision = cr;
  f->prog_if = cr >> 8;
  f->subclass = cr >> 16;
  f->class_code = cr >> 24;
  f->header_type = *(volatile uint8_t *)(c + PCI_HEADER_TYPE);
  f->parent_bus = parent;
  f->requester_id = IOMMU_PCI_ID(a->segment, bus, dev, fn);
  // Stop firmware-left-active DMA before probing or binding.
  uint16_t command = cfg16(c, PCI_COMMAND);
  cfg16_write(c, PCI_COMMAND,
              (command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                           PCI_COMMAND_MASTER)) |
                  PCI_COMMAND_INT_DISABLE);
  if (!walk_caps(f)) {
    print("pcid: malformed capability chain requester=");
    print_hex(f->requester_id);
    g_nfunctions--;
    return nullptr;
  }
  probe_bars(f);
  return f;
}

static void scan_bus(struct mcfg_allocation *a, uint8_t bus, uint8_t parent,
                     bool visited[256]) {
  if (bus < a->start_bus || bus > a->end_bus || visited[bus])
    return;
  visited[bus] = true;
  for (uint8_t dev = 0; dev < 32; dev++) {
    uint64_t base = a->base + ((uint64_t)(bus - a->start_bus) << 20) +
                    ((uint64_t)dev << 15);
    volatile uint8_t *c0 = (volatile uint8_t *)base;
    if (cfg16(c0, 0) == 0xffff)
      continue;
    uint8_t nfn = (*(volatile uint8_t *)(c0 + PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
    for (uint8_t fn = 0; fn < nfn; fn++) {
      struct pci_function *f = record_function(a, bus, dev, fn, parent);
      if (f == nullptr)
        continue;
      print("pcid: function requester=");
      print_hex(f->requester_id);
      print("pcid: class/rev=");
      print_hex(((uint64_t)f->class_code << 24) |
                ((uint64_t)f->subclass << 16) |
                ((uint64_t)f->prog_if << 8) | f->revision);
      if ((f->header_type & 0x7f) == 1) {
        uint8_t secondary = *(volatile uint8_t *)(f->config + 0x19);
        uint8_t subordinate = *(volatile uint8_t *)(f->config + 0x1a);
        if (secondary > bus && secondary <= subordinate &&
            subordinate <= a->end_bus)
          scan_bus(a, secondary, bus, visited);
      }
    }
  }
}

static bool parse_mcfg(const struct acpi_mcfg *mcfg) {
  if (mcfg->header.length < sizeof(*mcfg))
    return false;
  uint32_t bytes = mcfg->header.length - sizeof(*mcfg);
  if (bytes % sizeof(struct mcfg_allocation))
    return false;
  uint32_t count = bytes / sizeof(struct mcfg_allocation);
  if (count == 0 || count > MAX_MCFG_ALLOCS)
    return false;
  const struct mcfg_allocation *in = (const void *)(mcfg + 1);
  for (uint32_t i = 0; i < count; i++) {
    if (in[i].start_bus > in[i].end_bus || in[i].base % PAGE_SIZE)
      return false;
    uint64_t length = ((uint64_t)in[i].end_bus - in[i].start_bus + 1) << 20;
    if (in[i].base + length < in[i].base ||
        map_device(in[i].base, length,
                   VM_DEVICE_READ | VM_DEVICE_WRITE) != 0)
      return false;
    g_allocs[g_nallocs++] = in[i];
  }
  return true;
}

static struct pci_bar *bar_by_index(struct pci_function *f, uint8_t index) {
  for (uint8_t i = 0; i < f->n_bars; i++)
    if (f->bars[i].index == index)
      return &f->bars[i];
  return nullptr;
}

static bool allocate_msi(uint64_t *address, uint32_t *data,
                         struct cap_token *route) {
  return kring_irq_msi(&g_irq_control, &g_bootstrap->cap_irq, route,
                       address, data) == 0;
}

static bool program_interrupt_masked(struct managed_driver *m,
                                     uint64_t address, uint32_t data) {
  struct pci_function *f = m->function;
  if (f->msi_cap != 0) {
    uint8_t cap = f->msi_cap;
    uint16_t ctl = cfg16(f->config, cap + 2);
    cfg16_write(f->config, cap + 2, ctl & ~1u);
    cfg32_write(f->config, cap + 4, (uint32_t)address);
    uint32_t data_off;
    if (ctl & (1u << 7)) {
      cfg32_write(f->config, cap + 8, (uint32_t)(address >> 32));
      data_off = cap + 12;
    } else {
      data_off = cap + 8;
    }
    *(volatile uint16_t *)(f->config + data_off) = (uint16_t)data;
    m->interrupt_cap = cap;
    m->use_msix = false;
    return true;
  }
  if (f->msix_cap == 0)
    return false;
  uint8_t cap = f->msix_cap;
  uint16_t ctl = cfg16(f->config, cap + 2);
  uint32_t table = cfg32(f->config, cap + 4);
  struct pci_bar *bar = bar_by_index(f, table & 7);
  if (bar == nullptr)
    return false;
  uint64_t entry_address = bar->base + (table & ~7u);
  if (entry_address + 16 < entry_address || entry_address + 16 >
                                                bar->base + bar->size)
    return false;
  uint64_t page = page_floor(entry_address);
  // A table sharing a page with the controller BAR cannot be withheld from
  // the driver; use MSI above or reject this function.
  if (page >= m->bar_block &&
      page < m->bar_block + m->setup->bars[0].length)
    return false;
  if (map_device(page, PAGE_SIZE,
                 VM_DEVICE_READ | VM_DEVICE_WRITE) != 0)
    return false;
  m->table_block = page;
  m->msix_entry = (volatile uint32_t *)entry_address;
  cfg16_write(f->config, cap + 2, ctl | (1u << 15) | (1u << 14));
  m->msix_entry[3] = 1;
  m->msix_entry[0] = (uint32_t)address;
  m->msix_entry[1] = (uint32_t)(address >> 32);
  m->msix_entry[2] = data;
  atomic_thread_fence(memory_order_release);
  m->interrupt_cap = cap;
  m->use_msix = true;
  return true;
}

static void enable_interrupt(struct managed_driver *m) {
  uint16_t ctl = cfg16(m->function->config, m->interrupt_cap + 2);
  if (m->use_msix) {
    m->msix_entry[3] = 0;
    atomic_thread_fence(memory_order_release);
    cfg16_write(m->function->config, m->interrupt_cap + 2,
                (ctl | (1u << 15)) & ~(1u << 14));
  } else {
    cfg16_write(m->function->config, m->interrupt_cap + 2, ctl | 1u);
  }
}

static void mask_interrupt(struct managed_driver *m) {
  if (m->interrupt_cap == 0)
    return;
  uint16_t ctl = cfg16(m->function->config, m->interrupt_cap + 2);
  if (m->use_msix) {
    cfg16_write(m->function->config, m->interrupt_cap + 2,
                ctl | (1u << 14));
    if (m->msix_entry != nullptr)
      m->msix_entry[3] = 1;
  } else {
    cfg16_write(m->function->config, m->interrupt_cap + 2, ctl & ~1u);
  }
}

static bool block_request(struct gdos_block_channel *ch, uint32_t op,
                          uint64_t lba, uint32_t blocks, uint32_t offset,
                          uint32_t length) {
  uint32_t old = atomic_load_explicit(&ch->response_seq,
                                      memory_order_acquire);
  ch->op = op;
  ch->lba = lba;
  ch->block_count = blocks;
  ch->data_offset = offset;
  ch->data_length = length;
  uint32_t seq = old + 1;
  atomic_store_explicit(&ch->request_seq, seq, memory_order_release);
  if (sys_block_doorbell((uint64_t)ch) != 0)
    return false;
  for (;;) {
    uint32_t response = atomic_load_explicit(&ch->response_seq,
                                             memory_order_acquire);
    if (response == seq)
      return ch->status == GDOS_BLOCK_STATUS_OK;
    if (sys_block_wait(&ch->response_seq, response) != 0)
      return false;
  }
}

static void test_block_service(struct managed_driver *m) {
  struct gdos_block_channel *ch = (void *)m->service_block;
  if (!block_request(ch, GDOS_BLOCK_INFO, 0, 0, 0, 0) ||
      ch->logical_block_size == 0 ||
      ch->logical_block_size > PAGE_SIZE - 512) {
    print("pcid: NVMe block INFO request FAILED\n");
    return;
  }
  if (!block_request(ch, GDOS_BLOCK_READ, 0, 1, 512,
                     ch->logical_block_size)) {
    print("pcid: NVMe block READ request FAILED\n");
    return;
  }
  print("pcid: NVMe block protocol read succeeded\n");
}

static void prepare_nvme(struct pci_function *f) {
  if (f->class_code != 0x01 || f->subclass != 0x08 || f->prog_if != 0x02)
    return;
  print("pcid: matched NVMe requester=");
  print_hex(f->requester_id);
  print("pcid: MSI/MSIX caps=");
  print_hex(((uint64_t)f->msi_cap << 32) | f->msix_cap);
  for (uint8_t j = 0; j < f->n_bars; j++) {
    print("pcid: BAR index/base=");
    print_hex(((uint64_t)f->bars[j].index << 56) | f->bars[j].base);
    print("pcid: BAR size=");
    print_hex(f->bars[j].size);
  }
  for (uint8_t i = 0; i < f->n_bars; i++) {
    struct pci_bar *bar = &f->bars[i];
    if (bar->index != 0 || !bar->memory)
      continue;
    uint64_t start = page_floor(bar->base);
    uint64_t length = page_ceil(bar->base + bar->size) - start;
    // Withhold MSI-X table/PBA pages from the driver. QEMU places them in
    // BAR0 after the controller/doorbell pages; splitting the device-backed
    // ublock here preserves direct register access without exposing vectors.
    if (f->msix_cap != 0) {
      uint32_t table = cfg32(f->config, f->msix_cap + 4);
      uint32_t pba = cfg32(f->config, f->msix_cap + 8);
      uint64_t reserved = start + length;
      if ((table & 7) == bar->index)
        reserved = page_floor(bar->base + (table & ~7u));
      if ((pba & 7) == bar->index) {
        uint64_t pba_page = page_floor(bar->base + (pba & ~7u));
        if (pba_page < reserved)
          reserved = pba_page;
      }
      if (reserved <= start)
        return;
      if (reserved < start + length)
        length = reserved - start;
    }
    print("pcid: NVMe BAR0 base=");
    print_hex(start);
    print("pcid: NVMe BAR0 length=");
    print_hex(length);
    uint64_t rc = map_device(start, length,
                             VM_DEVICE_READ | VM_DEVICE_WRITE);
    print("pcid: mapped NVMe BAR0 rc=");
    print_hex(rc);
    if (rc != 0 || g_bootstrap->driver_image == 0)
      return;

    struct pci_driver_start *setup =
        (void *)sys_vm_alloc(PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
    if (sys_iserr((uint64_t)setup))
      return;
    memset(setup, 0, PAGE_SIZE);
    struct gdos_block_channel *service =
        (void *)sys_vm_alloc(PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
    if (sys_iserr((uint64_t)service)) {
      sys_vm_free((uint64_t)setup);
      return;
    }
    memset(service, 0, PAGE_SIZE);
    service->magic = GDOS_BLOCK_MAGIC;
    service->version = GDOS_BLOCK_VERSION;
    service->header_bytes = sizeof(*service);
    setup->version = PCI_DRIVER_START_VERSION;
    setup->n_bars = 1;
    setup->requester_id = f->requester_id;
    setup->function_id = ++g_driver_generation;
    setup->service_channel = (uint64_t)service;
    setup->bars[0] = (struct pci_driver_bar){
        .base = start,
        .length = length,
        .bar_index = bar->index,
        .flags = VM_DEVICE_READ | VM_DEVICE_WRITE,
    };
    if (kring_cap_subgrant(&g_cap_control, &g_bootstrap->cap_iommu,
          f->requester_id, 0, 0, KCAP_PARAM_P0, &setup->iommu_token) != 0) {
      sys_vm_free((uint64_t)service);
      sys_vm_free((uint64_t)setup);
      return;
    }

    // Memory decoding is required for the child to initialize its disabled
    // register image. Bus mastering remains clear until IOMMU+IRQ readiness.
    uint16_t command = cfg16(f->config, PCI_COMMAND);
    cfg16_write(f->config, PCI_COMMAND,
                (command | PCI_COMMAND_MEMORY | PCI_COMMAND_INT_DISABLE) &
                    ~PCI_COMMAND_MASTER);
    struct pe_resource resources[] = {
        {.base = start, .prot = VM_PROT_READ | VM_PROT_WRITE},
        {.base = (uint64_t)setup, .prot = VM_PROT_READ | VM_PROT_WRITE},
        {.base = (uint64_t)service, .prot = VM_PROT_READ | VM_PROT_WRITE},
    };
    uint64_t child = pe_spawn_resources(
        (const uint8_t *)g_bootstrap->driver_image,
        g_bootstrap->driver_image_length, (uint64_t)setup, 8 * PAGE_SIZE,
        resources, sizeof(resources) / sizeof(resources[0]));
    if (child == 0)
      return;
    g_driver = (struct managed_driver){.function = f,
                                       .setup = setup,
                                       .pid = child,
                                       .bar_block = start,
                                       .service_block = (uint64_t)service};
    if (!wait_state(setup, PCI_DRIVER_IOMMU_READY))
      return;
    uint64_t msi_address;
    uint32_t msi_data;
    if (!allocate_msi(&msi_address, &msi_data, &setup->irq_token)) {
      print("pcid: MSI allocation failed\n");
      sys_proc_kill(child);
      return;
    }
    if (!program_interrupt_masked(&g_driver, msi_address, msi_data)) {
      print("pcid: MSI programming failed\n");
      sys_proc_kill(child);
      return;
    }
    signal_state(setup, PCI_DRIVER_IRQ_GRANTED);
    if (!wait_state(setup, PCI_DRIVER_IRQ_READY))
      return;
    command = cfg16(f->config, PCI_COMMAND);
    cfg16_write(f->config, PCI_COMMAND,
                command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
                    PCI_COMMAND_INT_DISABLE);
    (void)cfg16(f->config, PCI_COMMAND);
    enable_interrupt(&g_driver);
    signal_state(setup, PCI_DRIVER_LIVE);
    print("pcid: NVMe live, BME enabled last\n");
    if (g_driver_generation > 1)
      test_block_service(&g_driver);
    return;
  }
}

static void reap_child(uint64_t pid) {
  for (;;) {
    uint64_t rc = sys_proc_reap(pid);
    if (rc == REAP_DONE)
      return;
    if (rc == SYSERR_AGAIN) {
      sys_yield();
      continue;
    }
    if (rc != REAP_MORE)
      return;
  }
}

static void driver_died(void) {
  struct pci_function *function = g_driver.function;
  mask_interrupt(&g_driver);
  volatile uint32_t *cc = (volatile uint32_t *)(g_driver.bar_block + 0x14);
  *cc = 0;
  uint16_t command = cfg16(g_driver.function->config, PCI_COMMAND);
  cfg16_write(g_driver.function->config, PCI_COMMAND,
              (command & ~(PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY)) |
                  PCI_COMMAND_INT_DISABLE);
  (void)cfg16(g_driver.function->config, PCI_COMMAND);
  reap_child(g_driver.pid);
  if (g_driver.table_block)
    sys_vm_free(g_driver.table_block);
  sys_vm_free(g_driver.bar_block);
  sys_vm_free(g_driver.service_block);
  sys_vm_free((uint64_t)g_driver.setup);
  memset(&g_driver, 0, sizeof(g_driver));
  print("pcid: dead driver disabled and reaped\n");
  if (g_driver_generation == 1) {
    print("pcid: restarting NVMe after death-path test\n");
    prepare_nvme(function);
  }
}

void _start(uint64_t bootstrap_address) {
  g_bootstrap = (const void *)bootstrap_address;
  if (g_bootstrap == nullptr || g_bootstrap->acpi_rsdp == 0)
    sys_proc_exit(1);
  uint64_t rsdp_address = g_bootstrap->acpi_rsdp;
  if (kring_cap_open(&g_cap_control, g_bootstrap->cap_channel, PAGE_SIZE) != 0)
    sys_proc_exit(1);
  print("pcid: starting, RSDP=");
  print_hex(rsdp_address);
  if (!map_firmware(rsdp_address, sizeof(struct acpi_rsdp))) {
    print("pcid: cannot map RSDP\n");
    sys_proc_exit(1);
  }
  const struct acpi_rsdp *rsdp = (const void *)rsdp_address;
  if (!sig(rsdp->signature, "RSD PTR ", 8) || !checksum_zero(rsdp, 20) ||
      (rsdp->revision >= 2 &&
       (rsdp->length < sizeof(*rsdp) ||
        !map_firmware(rsdp_address, rsdp->length) ||
        !checksum_zero(rsdp, rsdp->length)))) {
    print("pcid: invalid RSDP\n");
    sys_proc_exit(1);
  }
  const struct acpi_mcfg *mcfg =
      (const struct acpi_mcfg *)find_table(rsdp, "MCFG");
  if (mcfg == nullptr || !parse_mcfg(mcfg)) {
    print("pcid: invalid/missing MCFG\n");
    sys_proc_exit(1);
  }
  for (uint32_t i = 0; i < g_nallocs; i++) {
    bool visited[256] = {0};
    scan_bus(&g_allocs[i], g_allocs[i].start_bus, 0xff, visited);
  }
  print("pcid: enumerated function count=");
  print_hex(g_nfunctions);
  if (kring_create(&g_tree, KSCHEME_TREE, PAGE_SIZE) != 0 ||
      kring_create(&g_irq_control, KSCHEME_IRQ, PAGE_SIZE) != 0)
    sys_proc_exit(1);
  for (uint32_t i = 0; i < g_nfunctions; i++)
    prepare_nvme(&g_functions[i]);

  for (;;) {
    struct kcqe cqe;
    if (kring_wait_cqe(&g_tree, &cqe) == 0) {
      kring_ack(&g_tree);
      if (cqe.type == KEV_CHILD_DEAD && cqe.a == g_driver.pid)
        driver_died();
    }
    else
      sys_yield();
  }
}
