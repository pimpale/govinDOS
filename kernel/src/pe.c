#include "pe.h"

#include <stdint.h>

#include "debug.h"
#include "paging.h"
#include "stdlib/stdio.h"
#include "stdlib/string.h"
#include "thread.h"
#include "umem.h"

// Minimal on-disk PE32+ structures (only the fields we read).

struct [[gnu::packed]] dos_header {
  uint16_t e_magic; // 'MZ'
  uint8_t ignored[58];
  uint32_t e_lfanew; // offset of the PE signature
};

struct [[gnu::packed]] coff_header {
  uint32_t signature; // 'PE\0\0'
  uint16_t machine;   // 0x8664
  uint16_t n_sections;
  uint32_t timestamp;
  uint32_t symtab_off;
  uint32_t n_symbols;
  uint16_t opt_size;
  uint16_t characteristics;
};

struct [[gnu::packed]] data_directory {
  uint32_t va;
  uint32_t size;
};

struct [[gnu::packed]] optional_header64 {
  uint16_t magic; // 0x20B for PE32+
  uint8_t linker_major, linker_minor;
  uint32_t size_of_code, size_of_init_data, size_of_uninit_data;
  uint32_t entry_point; // RVA
  uint32_t base_of_code;
  uint64_t image_base;
  uint32_t section_align;
  uint32_t file_align;
  uint16_t os_major, os_minor, image_major, image_minor, subsys_major,
      subsys_minor;
  uint32_t win32_version;
  uint32_t size_of_image;   // total mapped size, section-aligned
  uint32_t size_of_headers; // file-aligned size of the header region
  uint32_t checksum;
  uint16_t subsystem;
  uint16_t dll_characteristics;
  uint64_t stack_reserve, stack_commit, heap_reserve, heap_commit;
  uint32_t loader_flags;
  uint32_t n_data_dirs;
  struct data_directory dirs[]; // n_data_dirs entries
};

struct [[gnu::packed]] section_header {
  char name[8];
  uint32_t virtual_size;
  uint32_t virtual_address; // RVA
  uint32_t raw_size;
  uint32_t raw_offset;
  uint32_t reloc_off, line_off;
  uint16_t n_relocs, n_lines;
  uint32_t characteristics;
};

#define PE_DIR_IMPORT 1
#define PE_DIR_BASERELOC 5
#define PE_DIR_TLS 9

#define SEC_EXEC 0x20000000u
#define SEC_READ 0x40000000u
#define SEC_WRITE 0x80000000u

#define RELOC_ABSOLUTE 0
#define RELOC_DIR64 10

#define PE_TLS_DIRECTORY_BYTES 40u
#define PE_TEB_TLS_VECTOR_OFFSET 0x58u
#define PE_TEB_SELF_OFFSET 0x30u
#define PE_TLS_VECTOR_OFFSET 0x80u
#define PE_TLS_DATA_MIN_OFFSET 0x100u

struct [[gnu::packed]] image_tls_directory64 {
  uint64_t start_address_of_raw_data;
  uint64_t end_address_of_raw_data;
  uint64_t address_of_index;
  uint64_t address_of_callbacks;
  uint32_t size_of_zero_fill;
  uint32_t characteristics;
};

static uint64_t page_ceil(uint64_t v) {
  return (v + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static bool range_inside(uint64_t base, uint64_t bytes, uint64_t address,
                         uint64_t length) {
  uint64_t image_end;
  uint64_t end;
  return !__builtin_add_overflow(base, bytes, &image_end) &&
         !__builtin_add_overflow(address, length, &end) && address >= base &&
         end <= image_end;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t pe_tls_create(struct process *p, uint64_t image_base,
                              const struct optional_header64 *opt) {
  uint64_t image_bytes = page_ceil(opt->size_of_image);
  if (opt->n_data_dirs <= PE_DIR_TLS ||
      opt->dirs[PE_DIR_TLS].size < PE_TLS_DIRECTORY_BYTES ||
      !range_inside(image_base, image_bytes,
                    image_base + opt->dirs[PE_DIR_TLS].va,
                    PE_TLS_DIRECTORY_BYTES)) {
    printf("pe: missing TLS directory\n");
    return 0;
  }
  struct image_tls_directory64 *tls =
      (void *)(image_base + opt->dirs[PE_DIR_TLS].va);
  uint64_t raw_bytes = tls->end_address_of_raw_data -
                       tls->start_address_of_raw_data;
  if (tls->start_address_of_raw_data > tls->end_address_of_raw_data ||
      tls->address_of_callbacks != 0 ||
      !range_inside(image_base, image_bytes, tls->start_address_of_raw_data,
                    raw_bytes) ||
      !range_inside(image_base, image_bytes, tls->address_of_index,
                    sizeof(uint32_t))) {
    printf("pe: unsupported TLS directory\n");
    return 0;
  }
  uint32_t align_code = (tls->characteristics >> 20) & 0xFu;
  if (align_code == 15) {
    return 0;
  }
  uint64_t alignment = align_code == 0 ? 1 : 1ull << (align_code - 1);
  if (alignment > PAGE_SIZE) {
    return 0;
  }
  uint64_t tls_bytes;
  if (__builtin_add_overflow(raw_bytes, (uint64_t)tls->size_of_zero_fill,
                             &tls_bytes) ||
      tls_bytes > (1ull << 20)) {
    return 0;
  }
  uint64_t data_offset = align_up(PE_TLS_DATA_MIN_OFFSET, alignment);
  uint64_t runtime_bytes;
  if (__builtin_add_overflow(data_offset, tls_bytes, &runtime_bytes)) {
    return 0;
  }
  uint8_t *runtime = umem_alloc(p, runtime_bytes, PAGE_R | PAGE_W);
  if (runtime == nullptr) {
    return 0;
  }
  uint64_t vector = (uint64_t)runtime + PE_TLS_VECTOR_OFFSET;
  uint64_t data = (uint64_t)runtime + data_offset;
  *(uint64_t *)(runtime + PE_TEB_SELF_OFFSET) = (uint64_t)runtime;
  *(uint64_t *)(runtime + PE_TEB_TLS_VECTOR_OFFSET) = vector;
  *(uint64_t *)vector = data;
  *(uint32_t *)tls->address_of_index = 0;
  memcpy((void *)data, (const void *)tls->start_address_of_raw_data,
         raw_bytes);
  return (uint64_t)runtime;
}

int pe_load(struct process *p, const uint8_t *image, size_t len,
            uint64_t *entry_out, uint64_t *gs_base_out) {
  if (len < sizeof(struct dos_header)) {
    return -1;
  }
  const struct dos_header *dos = (const void *)image;
  if (dos->e_magic != 0x5A4D /* MZ */ || dos->e_lfanew + 4 > len) {
    return -1;
  }
  const struct coff_header *coff = (const void *)(image + dos->e_lfanew);
  if (coff->signature != 0x00004550 /* PE\0\0 */ || coff->machine != 0x8664) {
    return -1;
  }
  const struct optional_header64 *opt = (const void *)(coff + 1);
  if (opt->magic != 0x20B) {
    return -1;
  }
  if (opt->section_align != PAGE_SIZE) {
    printf("pe: unsupported section alignment %u\n", opt->section_align);
    return -1;
  }
  // No import resolution: reject rather than jump to unresolved stubs.
  if (opt->n_data_dirs > PE_DIR_IMPORT &&
      opt->dirs[PE_DIR_IMPORT].size != 0) {
    printf("pe: image has imports; static executables only\n");
    return -1;
  }

  const struct section_header *sections =
      (const void *)((const uint8_t *)opt + coff->opt_size);
  uint64_t image_size = page_ceil(opt->size_of_image);

  // One contiguous PAGE_U region for the whole image. Mapped RW for the
  // copy; per-section protections are applied below (W^X).
  uint8_t *base = umem_alloc(p, image_size, PAGE_R | PAGE_W);
  if (base == nullptr) {
    return -1;
  }

  // Headers, then each section's raw data (calloc already zeroed the
  // VirtualSize > raw_size tail, i.e. .bss).
  memcpy(base, image, opt->size_of_headers);
  for (uint16_t i = 0; i < coff->n_sections; i++) {
    const struct section_header *s = &sections[i];
    if ((uint64_t)s->virtual_address + s->raw_size > image_size ||
        (uint64_t)s->raw_offset + s->raw_size > len) {
      return -1;
    }
    memcpy(base + s->virtual_address, image + s->raw_offset, s->raw_size);
  }

  // Base relocations. Every process loads at a fresh address in the
  // shared AS, so the delta is essentially never zero. An *empty* reloc
  // directory is fine as long as the COFF header doesn't declare
  // IMAGE_FILE_RELOCS_STRIPPED — small PIC-friendly images legitimately
  // need zero fixups (everything rip-relative).
  if ((coff->characteristics & 0x0001 /* RELOCS_STRIPPED */) != 0) {
    printf("pe: image not relocatable (linked /fixed?)\n");
    return -1;
  }
  uint64_t delta = (uint64_t)base - opt->image_base;
  if (delta != 0 && opt->n_data_dirs > PE_DIR_BASERELOC &&
      opt->dirs[PE_DIR_BASERELOC].size != 0) {
    const uint8_t *rel = base + opt->dirs[PE_DIR_BASERELOC].va;
    const uint8_t *rel_end = rel + opt->dirs[PE_DIR_BASERELOC].size;
    while (rel + 8 <= rel_end) {
      uint32_t page_rva = *(const uint32_t *)rel;
      uint32_t block_size = *(const uint32_t *)(rel + 4);
      if (block_size < 8) {
        return -1;
      }
      const uint16_t *entry = (const uint16_t *)(rel + 8);
      size_t n = (block_size - 8) / 2;
      for (size_t e = 0; e < n; e++) {
        uint16_t type = entry[e] >> 12;
        uint16_t off = entry[e] & 0xFFF;
        if (type == RELOC_DIR64) {
          *(uint64_t *)(base + page_rva + off) += delta;
        } else if (type != RELOC_ABSOLUTE) {
          printf("pe: unsupported reloc type %u\n", type);
          return -1;
        }
      }
      rel += block_size;
    }
  }

  uint64_t gs_base = pe_tls_create(p, (uint64_t)base, opt);
  if (gs_base == 0) {
    return -1;
  }

  // Per-section W^X. The header page stays read-only.
  as_flag(p->as, (uint64_t)base, (uint64_t)base + PAGE_SIZE, PAGE_R | PAGE_U);
  for (uint16_t i = 0; i < coff->n_sections; i++) {
    const struct section_header *s = &sections[i];
    paging_flags_t f = PAGE_U;
    if (s->characteristics & SEC_READ) {
      f |= PAGE_R;
    }
    if (s->characteristics & SEC_WRITE) {
      f |= PAGE_W;
    }
    if (s->characteristics & SEC_EXEC) {
      f |= PAGE_X;
    }
    uint64_t start = (uint64_t)base + s->virtual_address;
    uint64_t end = start + page_ceil(s->virtual_size ? s->virtual_size
                                                     : s->raw_size);
    as_flag(p->as, start, end, f);
  }
  as_flush(p->as);

  *entry_out = (uint64_t)base + opt->entry_point;
  *gs_base_out = gs_base;
  return 0;
}
