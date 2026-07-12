#include "iommu_internal.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi.h"
#include "debug.h"
#include "iommu.h"
#include "irq_scheme.h"
#include "lapic.h"
#include "paging.h"
#include "platform_mem.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"

#include <gdos/kring_iommu.h>

#define VTD_REG_CAP    0x08
#define VTD_REG_ECAP   0x10
#define VTD_REG_GCMD   0x18
#define VTD_REG_GSTS   0x1C
#define VTD_REG_RTADDR 0x20
#define VTD_REG_CCMD   0x28
#define VTD_REG_FSTS   0x34
#define VTD_REG_FECTL  0x38
#define VTD_REG_FEDATA 0x3C
#define VTD_REG_FEADDR 0x40
#define VTD_REG_FEUADDR 0x44

#define VTD_GCMD_TE   (1u << 31)
#define VTD_GCMD_SRTP (1u << 30)
#define VTD_GSTS_TES  (1u << 31)
#define VTD_GSTS_RTPS (1u << 30)

#define VTD_CCMD_ICC         (1ull << 63)
#define VTD_CCMD_GLOBAL      (1ull << 61)
#define VTD_IOTLB_IVT        (1ull << 63)
#define VTD_IOTLB_GLOBAL     (1ull << 60)
#define VTD_CONTEXT_PRESENT  (1ull << 0)
#define VTD_ROOT_PRESENT     (1ull << 0)
#define VTD_SL_READ          (1ull << 0)
#define VTD_SL_WRITE         (1ull << 1)
#define VTD_SL_PS            (1ull << 7)
#define VTD_ADDR_MASK        0x000FFFFFFFFFF000ull

#define VTD_WAIT_LIMIT 10000000u

struct acpi_dmar {
  struct acpi_sdt_header header;
  uint8_t host_address_width;
  uint8_t flags;
  uint8_t reserved[10];
} __attribute__((packed));

struct dmar_entry {
  uint16_t type;
  uint16_t length;
} __attribute__((packed));

struct dmar_drhd {
  struct dmar_entry header;
  uint8_t flags;
  uint8_t reserved;
  uint16_t segment;
  uint64_t register_base;
} __attribute__((packed));

struct vtd_context_entry {
  uint64_t lo;
  uint64_t hi;
};

static volatile uint8_t *g_regs;
static uint64_t g_cap, g_ecap;
static uint64_t *g_root;
static struct vtd_context_entry *g_contexts[256];
static uint32_t g_gcmd;
static uint16_t g_next_domain = 1;
static bool g_domain_used[1u << 16];
static bool g_superpage_2m;
static uint32_t g_fault_offset, g_fault_records;

static uint32_t read32(uint32_t off) {
  return *(volatile uint32_t *)(g_regs + off);
}
static uint64_t read64(uint32_t off) {
  return *(volatile uint64_t *)(g_regs + off);
}
static void write32(uint32_t off, uint32_t value) {
  *(volatile uint32_t *)(g_regs + off) = value;
}
static void write64(uint32_t off, uint64_t value) {
  *(volatile uint64_t *)(g_regs + off) = value;
}

static bool wait32(uint32_t off, uint32_t mask, bool set) {
  for (uint32_t i = 0; i < VTD_WAIT_LIMIT; i++) {
    if (((read32(off) & mask) != 0) == set)
      return true;
    __asm__ volatile("pause");
  }
  return false;
}

static bool wait64_clear(uint32_t off, uint64_t mask) {
  for (uint32_t i = 0; i < VTD_WAIT_LIMIT; i++) {
    if ((read64(off) & mask) == 0)
      return true;
    __asm__ volatile("pause");
  }
  return false;
}

static bool invalidate_all(void) {
  atomic_thread_fence(memory_order_release);
  write64(VTD_REG_CCMD, VTD_CCMD_ICC | VTD_CCMD_GLOBAL);
  if (!wait64_clear(VTD_REG_CCMD, VTD_CCMD_ICC))
    return false;
  uint32_t iotlb = (uint32_t)(((g_ecap >> 8) & 0x3ff) * 16);
  write64(iotlb + 8, VTD_IOTLB_IVT | VTD_IOTLB_GLOBAL);
  return wait64_clear(iotlb + 8, VTD_IOTLB_IVT);
}

static void free_slpt(uint64_t *table, int level) {
  if (table == nullptr)
    return;
  if (level > 1)
    for (uint32_t i = 0; i < 512; i++)
      if ((table[i] & (VTD_SL_READ | VTD_SL_WRITE)) &&
          !(table[i] & VTD_SL_PS))
        free_slpt((uint64_t *)(table[i] & VTD_ADDR_MASK), level - 1);
  free(table);
}

static uint64_t *slpt_entry(struct iommu_hw_domain *domain, uint64_t iova,
                            int target_level, bool allocate) {
  uint64_t *table = domain->root;
  for (int level = 4; level > target_level; level--) {
    uint32_t idx = (uint32_t)((iova >> (12 + 9 * (level - 1))) & 0x1ff);
    if (!(table[idx] & (VTD_SL_READ | VTD_SL_WRITE))) {
      if (!allocate)
        return nullptr;
      uint64_t *child = malloc(PAGE_SIZE);
      if (child == nullptr)
        return nullptr;
      memset(child, 0, PAGE_SIZE);
      table[idx] = ((uint64_t)child & VTD_ADDR_MASK) | VTD_SL_READ |
                   VTD_SL_WRITE;
    }
    table = (uint64_t *)(table[idx] & VTD_ADDR_MASK);
  }
  uint32_t shift = target_level == 2 ? 21 : 12;
  return &table[(iova >> shift) & 0x1ff];
}

bool vtd_init_required(const struct acpi_rsdp *rsdp) {
  const struct acpi_dmar *dmar =
      (const struct acpi_dmar *)acpi_find_table(rsdp, "DMAR");
  if (dmar == nullptr) {
    printf("vtd: DMAR missing\n");
    return false;
  }
  printf("vtd: DMAR len=%u haw=%u flags=%u\n", dmar->header.length,
         dmar->host_address_width, dmar->flags);
  if (dmar->header.length < sizeof(*dmar) ||
      dmar->host_address_width < 38)
    return false;

  const struct dmar_drhd *chosen = nullptr;
  const uint8_t *p = (const uint8_t *)(dmar + 1);
  const uint8_t *end = (const uint8_t *)dmar + dmar->header.length;
  while (p < end) {
    if ((uint64_t)(end - p) < sizeof(struct dmar_entry))
      return false;
    const struct dmar_entry *e = (const struct dmar_entry *)p;
    printf("vtd: DMAR entry type=%u len=%u\n", e->type, e->length);
    if (e->length < sizeof(*e) || e->length > (uint64_t)(end - p))
      return false;
    if (e->type == 0) {
      if (e->length < sizeof(struct dmar_drhd))
        return false;
      const struct dmar_drhd *drhd = (const struct dmar_drhd *)p;
      printf("vtd: DRHD flags=%u seg=%u reg=%016llX\n", drhd->flags,
             drhd->segment, drhd->register_base);
      // V1 deliberately supports one segment-zero unit. Current QEMU emits
      // IOAPIC/HPET scopes rather than INCLUDE_PCI_ALL even though the sole
      // emulated unit remaps every PCI requester, so the single-unit virtual
      // platform is the other accepted representation.
      if (drhd->segment != 0 || chosen != nullptr)
        return false;
      chosen = drhd;
    } else if (e->type == 1) {
      return false; // RMRR support is intentionally fail-closed.
    }
    p += e->length;
  }
  if (chosen == nullptr || chosen->register_base % PAGE_SIZE != 0)
    return false;

  g_regs = (volatile uint8_t *)(uintptr_t)chosen->register_base;
  if (!platform_mem_protect(chosen->register_base, PAGE_SIZE))
    return false;
  as_flag(g_as_kernel, chosen->register_base,
          chosen->register_base + PAGE_SIZE, PAGE_R | PAGE_W | PAGE_UC);
  as_flush(g_as_kernel);
  g_cap = read64(VTD_REG_CAP);
  g_ecap = read64(VTD_REG_ECAP);
  printf("vtd: raw cap=%016llX ecap=%016llX\n", g_cap, g_ecap);
  // Four-level second-level translation (SAGAW bit 2) is mandatory.
  if (!(g_cap & (1ull << 10))) {
    printf("vtd: four-level SAGAW unavailable\n");
    return false;
  }
  g_fault_offset = (uint32_t)(((g_cap >> 24) & 0x3ff) * 16);
  g_fault_records = (uint32_t)(((g_cap >> 40) & 0xff) + 1);
  if (g_fault_records > 256)
    return false;
  g_superpage_2m = ((g_cap >> 34) & 1) != 0;

  g_root = malloc(PAGE_SIZE);
  if (g_root == nullptr)
    return false;
  memset(g_root, 0, PAGE_SIZE);
  write64(VTD_REG_RTADDR, (uint64_t)g_root & VTD_ADDR_MASK);
  g_gcmd = VTD_GCMD_SRTP;
  write32(VTD_REG_GCMD, g_gcmd);
  if (!wait32(VTD_REG_GSTS, VTD_GSTS_RTPS, true)) {
    printf("vtd: set-root timeout gsts=%08X\n", read32(VTD_REG_GSTS));
    return false;
  }
  if (!invalidate_all()) {
    printf("vtd: initial invalidation timeout\n");
    return false;
  }
  write32(VTD_REG_FSTS, read32(VTD_REG_FSTS));
  write32(VTD_REG_FEADDR,
          0xFEE00000u | ((uint32_t)x86_lapic_id() << 12));
  write32(VTD_REG_FEUADDR, 0);
  write32(VTD_REG_FEDATA, VECTOR_IOMMU_FAULT);
  write32(VTD_REG_FECTL, 0); // clear interrupt-mask bit
  g_gcmd |= VTD_GCMD_TE;
  write32(VTD_REG_GCMD, g_gcmd);
  if (!wait32(VTD_REG_GSTS, VTD_GSTS_TES, true)) {
    printf("vtd: translation-enable timeout gsts=%08X\n",
           read32(VTD_REG_GSTS));
    return false;
  }
  printf("vtd: cap=%016llX ecap=%016llX reg=%016llX\n", g_cap, g_ecap,
         chosen->register_base);
  return true;
}

bool vtd_domain_init(struct iommu_hw_domain *domain) {
  uint16_t start = g_next_domain;
  while (g_domain_used[g_next_domain]) {
    g_next_domain++;
    if (g_next_domain == 0)
      g_next_domain = 1;
    if (g_next_domain == start)
      return false;
  }
  uint16_t id = g_next_domain;
  g_domain_used[id] = true;
  g_next_domain++;
  if (g_next_domain == 0)
    g_next_domain = 1;
  if (id == 0)
    return false;
  domain->root = malloc(PAGE_SIZE);
  if (domain->root == nullptr) {
    g_domain_used[id] = false;
    return false;
  }
  memset(domain->root, 0, PAGE_SIZE);
  domain->id = id;
  return true;
}

void vtd_domain_destroy(struct iommu_hw_domain *domain) {
  free_slpt(domain->root, 4);
  domain->root = nullptr;
  g_domain_used[domain->id] = false;
  domain->id = 0;
}

bool vtd_covers(struct iommu_device_id id) {
  return g_regs != nullptr && id.segment == 0;
}

static struct vtd_context_entry *context_for(struct iommu_device_id id,
                                             bool allocate) {
  if (g_contexts[id.bus] == nullptr && allocate) {
    struct vtd_context_entry *ctx = malloc(PAGE_SIZE);
    if (ctx == nullptr)
      return nullptr;
    memset(ctx, 0, PAGE_SIZE);
    g_contexts[id.bus] = ctx;
    g_root[id.bus] = ((uint64_t)ctx & VTD_ADDR_MASK) | VTD_ROOT_PRESENT;
  }
  return g_contexts[id.bus] != nullptr ? &g_contexts[id.bus][id.devfn]
                                       : nullptr;
}

bool vtd_attach(struct iommu_hw_domain *domain, struct iommu_device_id id) {
  struct vtd_context_entry *ctx = context_for(id, true);
  if (ctx == nullptr || (ctx->lo & VTD_CONTEXT_PRESENT))
    return false;
  // Translation type 0 (multi-level), AW=2 (48-bit/4-level).
  ctx->hi = ((uint64_t)domain->id << 8) | 2;
  atomic_thread_fence(memory_order_release);
  ctx->lo = ((uint64_t)domain->root & VTD_ADDR_MASK) | VTD_CONTEXT_PRESENT;
  if (!invalidate_all())
    fatal("vtd: runtime context invalidation timeout\n");
  return true;
}

bool vtd_detach(struct iommu_hw_domain *domain, struct iommu_device_id id) {
  (void)domain;
  struct vtd_context_entry *ctx = context_for(id, false);
  if (ctx == nullptr || !(ctx->lo & VTD_CONTEXT_PRESENT))
    return false;
  ctx->lo = 0;
  ctx->hi = 0;
  if (!invalidate_all())
    fatal("vtd: runtime detach invalidation timeout\n");
  return true;
}

uint64_t vtd_mapping_leaves(uint64_t iova, uint64_t pages) {
  uint64_t leaves = 0;
  while (pages != 0) {
    if (g_superpage_2m && (iova & ((2ull << 20) - 1)) == 0 && pages >= 512) {
      iova += 512 * PAGE_SIZE;
      pages -= 512;
    } else {
      iova += PAGE_SIZE;
      pages--;
    }
    leaves++;
  }
  return leaves;
}

bool vtd_map(struct iommu_hw_domain *domain, uint64_t iova, uint64_t phys,
             uint64_t pages, uint32_t permissions) {
  uint64_t bits = 0;
  if (permissions & 1)
    bits |= VTD_SL_READ;
  if (permissions & 2)
    bits |= VTD_SL_WRITE;
  if (iova >= (1ull << 48) || phys >= (1ull << 48) ||
      pages > ((1ull << 48) - iova) / PAGE_SIZE)
    return false;
  uint64_t mapped = 0;
  while (mapped < pages) {
    bool huge = g_superpage_2m &&
                ((iova + mapped * PAGE_SIZE) & ((2ull << 20) - 1)) == 0 &&
                ((phys + mapped * PAGE_SIZE) & ((2ull << 20) - 1)) == 0 &&
                pages - mapped >= 512;
    uint64_t *leaf = slpt_entry(domain, iova + mapped * PAGE_SIZE,
                                huge ? 2 : 1, true);
    if (leaf == nullptr || (*leaf & (VTD_SL_READ | VTD_SL_WRITE)))
      break;
    *leaf = ((phys + mapped * PAGE_SIZE) & VTD_ADDR_MASK) | bits |
            (huge ? VTD_SL_PS : 0);
    mapped += huge ? 512 : 1;
  }
  if (mapped != pages) {
    for (uint64_t i = 0; i < mapped;) {
      bool huge = g_superpage_2m &&
                  ((iova + i * PAGE_SIZE) & ((2ull << 20) - 1)) == 0 &&
                  mapped - i >= 512;
      uint64_t *leaf = slpt_entry(domain, iova + i * PAGE_SIZE,
                                  huge ? 2 : 1, false);
      if (leaf != nullptr)
        *leaf = 0;
      i += huge ? 512 : 1;
    }
    if (!invalidate_all())
      fatal("vtd: rollback invalidation timeout\n");
    return false;
  }
  if (!invalidate_all())
    fatal("vtd: runtime map invalidation timeout\n");
  return true;
}

bool vtd_unmap(struct iommu_hw_domain *domain, uint64_t iova, uint64_t pages) {
  for (uint64_t i = 0; i < pages;) {
    bool huge = g_superpage_2m &&
                ((iova + i * PAGE_SIZE) & ((2ull << 20) - 1)) == 0 &&
                pages - i >= 512;
    uint64_t *leaf = slpt_entry(domain, iova + i * PAGE_SIZE,
                                huge ? 2 : 1, false);
    if (leaf == nullptr || !(*leaf & (VTD_SL_READ | VTD_SL_WRITE)))
      return false;
    *leaf = 0;
    i += huge ? 512 : 1;
  }
  if (!invalidate_all())
    fatal("vtd: runtime unmap invalidation timeout\n");
  return true;
}

void vtd_fault_interrupt(void) {
  // One register per hardware fault, bounded by CAP.NFR. Valid (F) is
  // write-one-to-clear. Legacy records encode reason in hi[39:32], source
  // id in hi[15:0], request type in hi[62], and the page address in lo.
  for (uint32_t i = 0; i < g_fault_records; i++) {
    uint32_t off = g_fault_offset + i * 16;
    uint64_t hi = read64(off + 8);
    if (!(hi & (1ull << 63)))
      continue;
    uint64_t iova = read64(off) & ~0xfffull;
    uint16_t source = (uint16_t)hi;
    uint8_t hw_reason = (uint8_t)(hi >> 32);
    uint32_t reason = IOMMU_FAULT_PTE_MISSING;
    if (hw_reason == 1 || hw_reason == 2)
      reason = IOMMU_FAULT_CONTEXT_MISSING;
    if (hi & (1ull << 62))
      reason |= IOMMU_FAULT_ACCESS_WRITE;
    iommu_report_fault(source, iova, reason);
    write64(off + 8, hi | (1ull << 63));
  }
  uint32_t fsts = read32(VTD_REG_FSTS);
  if (fsts)
    write32(VTD_REG_FSTS, fsts);
}
