#include "acpi.h"

#include <stddef.h>
#include <stdint.h>

#include <efi/efi.h>
#include <efi/types.h>

#include "stdlib/stdio.h"
#include "stdlib/string.h"

// {8868E871-E4F1-11D3-BC22-0080C73C8881}
static const struct efi_guid acpi_20_guid = {
    0x8868E871, 0xE4F1, 0x11D3,
    {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};

// {EB9D2D30-2D88-11D3-9A16-0090273FC14D}
static const struct efi_guid acpi_10_guid = {
    0xEB9D2D30, 0x2D88, 0x11D3,
    {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

static bool guid_eq(const struct efi_guid *a, const struct efi_guid *b) {
  if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3) {
    return false;
  }
  for (int i = 0; i < 8; i++) {
    if (a->data4[i] != b->data4[i]) {
      return false;
    }
  }
  return true;
}

static bool checksum_zero(const void *p, size_t len) {
  const uint8_t *b = (const uint8_t *)p;
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += b[i];
  }
  return sum == 0;
}

static bool sig_eq(const char *a, const char *b, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

static bool rsdp_valid(const struct acpi_rsdp *r) {
  if (!sig_eq(r->signature, "RSD PTR ", 8)) {
    return false;
  }
  if (!checksum_zero(r, 20)) {
    return false;
  }
  if (r->revision >= 2 && !checksum_zero(r, r->length)) {
    return false;
  }
  return true;
}

static bool sdt_valid(const struct acpi_sdt_header *h, const char *signature) {
  if (!sig_eq(h->signature, signature, 4)) {
    return false;
  }
  return checksum_zero(h, h->length);
}

const struct acpi_rsdp *acpi_init(struct efi_system_table *system) {
  const struct efi_configuration_table *ct = system->configuration_table;
  efi_uint_t n = system->number_of_table_entries;

  const struct acpi_rsdp *rsdp_v1 = nullptr;
  const struct acpi_rsdp *rsdp_v2 = nullptr;

  for (efi_uint_t i = 0; i < n; i++) {
    if (guid_eq(&ct[i].vendor_guid, &acpi_20_guid)) {
      rsdp_v2 = (const struct acpi_rsdp *)ct[i].vendor_table;
    } else if (guid_eq(&ct[i].vendor_guid, &acpi_10_guid)) {
      rsdp_v1 = (const struct acpi_rsdp *)ct[i].vendor_table;
    }
  }

  const struct acpi_rsdp *r = rsdp_v2 ? rsdp_v2 : rsdp_v1;
  if (r == nullptr) {
    printf("acpi: no RSDP in EFI configuration table\n");
    return nullptr;
  }
  if (!rsdp_valid(r)) {
    printf("acpi: RSDP at %016llX failed validation\n", (uint64_t)r);
    return nullptr;
  }

  printf("acpi: RSDP %016llX rev=%u\n", (uint64_t)r, r->revision);
  return r;
}

const struct acpi_madt *acpi_find_madt(const struct acpi_rsdp *rsdp) {
  if (rsdp == nullptr) {
    return nullptr;
  }

  // Pick XSDT when available; the XSDT entry array is uint64_t per pointer,
  // RSDT is uint32_t. The arrays are not guaranteed to be naturally aligned
  // (XSDT array starts at offset 36), so read pointers via memcpy.
  const struct acpi_sdt_header *root;
  size_t entry_size;

  if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
    root = (const struct acpi_sdt_header *)(uintptr_t)rsdp->xsdt_address;
    entry_size = 8;
    if (!sdt_valid(root, "XSDT")) {
      return nullptr;
    }
  } else {
    root = (const struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_address;
    entry_size = 4;
    if (!sdt_valid(root, "RSDT")) {
      return nullptr;
    }
  }

  const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
  size_t n_entries =
      (root->length - sizeof(*root)) / entry_size;

  for (size_t i = 0; i < n_entries; i++) {
    uint64_t addr = 0;
    memcpy(&addr, entries + i * entry_size, entry_size);

    const struct acpi_sdt_header *h =
        (const struct acpi_sdt_header *)(uintptr_t)addr;
    if (sig_eq(h->signature, "APIC", 4) && checksum_zero(h, h->length)) {
      return (const struct acpi_madt *)h;
    }
  }
  return nullptr;
}

