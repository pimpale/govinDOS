#include "platform_mem.h"

#include <stddef.h>

#include "debug.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"

#include <gdosabi/syscall.h>

#define PLATFORM_MAX_PROTECTED 32
#define PLATFORM_MAX_ECAM 8

struct range {
  uint64_t base;
  uint64_t end;
};

struct acpi_mcfg {
  struct acpi_sdt_header header;
  uint64_t reserved;
} __attribute__((packed));

struct acpi_mcfg_allocation {
  uint64_t base;
  uint16_t segment;
  uint8_t start_bus;
  uint8_t end_bus;
  uint32_t reserved;
} __attribute__((packed));

static struct efi_memory_descriptor *g_mmap;
static uint64_t g_mmap_len;
static struct range g_protected[PLATFORM_MAX_PROTECTED];
static uint32_t g_nprotected;
static struct range g_ecam[PLATFORM_MAX_ECAM];
static uint32_t g_necam;

static bool range_end(uint64_t base, uint64_t length, uint64_t *end) {
  return length != 0 && base % PAGE_SIZE == 0 && length % PAGE_SIZE == 0 &&
         !__builtin_add_overflow(base, length, end);
}

static bool overlaps(uint64_t base, uint64_t end, const struct range *r) {
  return base < r->end && r->base < end;
}

static bool contained(uint64_t base, uint64_t end, const struct range *r) {
  return base >= r->base && end <= r->end;
}

static bool efi_type_contains(uint32_t type, uint64_t base, uint64_t end) {
  for (uint64_t i = 0; i < g_mmap_len; i++) {
    uint64_t mend;
    if (__builtin_mul_overflow(g_mmap[i].pages, (uint64_t)PAGE_SIZE, &mend) ||
        __builtin_add_overflow(g_mmap[i].physical_start, mend, &mend))
      continue;
    if (g_mmap[i].type == type && base >= g_mmap[i].physical_start &&
        end <= mend)
      return true;
  }
  return false;
}

static bool overlaps_usable(uint64_t base, uint64_t end) {
  for (uint64_t i = 0; i < g_mmap_len; i++) {
    if (g_mmap[i].type != EFI_CONVENTIONAL_MEMORY)
      continue;
    uint64_t bytes, mend;
    if (__builtin_mul_overflow(g_mmap[i].pages, (uint64_t)PAGE_SIZE, &bytes) ||
        __builtin_add_overflow(g_mmap[i].physical_start, bytes, &mend))
      return true;
    struct range r = {.base = g_mmap[i].physical_start, .end = mend};
    if (overlaps(base, end, &r))
      return true;
  }
  return false;
}

void platform_mem_init(uint64_t n_mmap,
                       const struct efi_memory_descriptor *mmap,
                       const struct acpi_rsdp *rsdp) {
  asserts(g_mmap == nullptr, "platform_mem: initialized twice");
  g_mmap = malloc(n_mmap * sizeof(*g_mmap));
  asserts(g_mmap != nullptr, "platform_mem: map snapshot allocation failed");
  memcpy(g_mmap, mmap, n_mmap * sizeof(*g_mmap));
  g_mmap_len = n_mmap;

  const struct acpi_mcfg *mcfg =
      (const struct acpi_mcfg *)acpi_find_table(rsdp, "MCFG");
  if (mcfg == nullptr)
    return;
  uint64_t bytes = mcfg->header.length - sizeof(*mcfg);
  if (bytes % sizeof(struct acpi_mcfg_allocation) != 0)
    return;
  const struct acpi_mcfg_allocation *a =
      (const struct acpi_mcfg_allocation *)(mcfg + 1);
  for (uint64_t i = 0; i < bytes / sizeof(*a) && g_necam < PLATFORM_MAX_ECAM;
       i++) {
    if (a[i].start_bus > a[i].end_bus || a[i].base % PAGE_SIZE != 0)
      continue;
    uint64_t length = ((uint64_t)a[i].end_bus - a[i].start_bus + 1) << 20;
    uint64_t end;
    if (!range_end(a[i].base, length, &end))
      continue;
    g_ecam[g_necam++] = (struct range){a[i].base, end};
  }
}

bool platform_mem_protect(uint64_t base, uint64_t length) {
  uint64_t end;
  if (g_nprotected == PLATFORM_MAX_PROTECTED ||
      !range_end(base, length, &end))
    return false;
  g_protected[g_nprotected++] = (struct range){base, end};
  return true;
}

bool platform_mem_validate_device(uint64_t base, uint64_t length,
                                  uint32_t flags,
                                  paging_flags_t *kernel_flags_out,
                                  bool *delegatable_out) {
  uint64_t end;
  if (!range_end(base, length, &end) ||
      (flags & ~(VM_DEVICE_READ | VM_DEVICE_WRITE | VM_DEVICE_WC |
                 VM_DEVICE_FIRMWARE)) != 0 ||
      !(flags & VM_DEVICE_READ) || (flags & VM_DEVICE_WC))
    return false;
  for (uint32_t i = 0; i < g_nprotected; i++)
    if (overlaps(base, end, &g_protected[i]))
      return false;

  if (flags & VM_DEVICE_FIRMWARE) {
    if (flags & VM_DEVICE_WRITE)
      return false;
    bool acpi = efi_type_contains(EFI_ACPI_RECLAIM_MEMORY, base, end) ||
                efi_type_contains(EFI_ACPI_MEMORY_NVS, base, end);
    if (!acpi)
      return false;
    *kernel_flags_out = PAGE_KERNEL_PRISTINE;
    *delegatable_out = false;
    return true;
  }
  if (overlaps_usable(base, end))
    return false;
  for (uint32_t i = 0; i < g_necam; i++) {
    if (overlaps(base, end, &g_ecam[i])) {
      if (!contained(base, end, &g_ecam[i]))
        return false;
      *kernel_flags_out = PAGE_R | PAGE_W | PAGE_X | PAGE_UC;
      *delegatable_out = false;
      return true;
    }
  }

  // Q35's fixed 32-bit PCI MMIO aperture. Real hardware must replace this
  // platform fact with host-bridge _CRS windows before it is supported.
  struct range q35 = {.base = 0xC0000000ull, .end = 0xFEC00000ull};
  struct range q35_high = {.base = 0x800000000ull, .end = 0x1000000000ull};
  if (!contained(base, end, &q35) && !contained(base, end, &q35_high) &&
      !efi_type_contains(EFI_MEMORY_MAPPED_IO, base, end))
    return false;
  *kernel_flags_out = PAGE_R | PAGE_W | PAGE_X | PAGE_UC;
  *delegatable_out = true;
  return true;
}
