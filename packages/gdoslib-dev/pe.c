#include "pe.h"

#include <string.h>

#include "sys.h"

// Minimal on-disk PE32+ structures (only the fields we read) — the same
// shapes as kernel/src/pe.c.

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

#define UPAGE_SIZE 4096ull
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
  return (v + UPAGE_SIZE - 1) & ~(UPAGE_SIZE - 1);
}

static uint64_t fail(const char *why) {
  print("pe: ");
  print(why);
  print("\n");
  return 0;
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

uint64_t pe_tls_create(uint64_t image_base) {
  const struct dos_header *dos = (const void *)image_base;
  if (dos->e_magic != 0x5A4D) {
    return fail("TLS image has no DOS header");
  }
  const struct coff_header *coff =
      (const void *)(image_base + dos->e_lfanew);
  const struct optional_header64 *opt = (const void *)(coff + 1);
  uint64_t image_bytes = page_ceil(opt->size_of_image);
  if (coff->signature != 0x00004550 || opt->magic != 0x20B ||
      opt->n_data_dirs <= PE_DIR_TLS ||
      opt->dirs[PE_DIR_TLS].size < PE_TLS_DIRECTORY_BYTES ||
      !range_inside(image_base, image_bytes,
                    image_base + opt->dirs[PE_DIR_TLS].va,
                    PE_TLS_DIRECTORY_BYTES)) {
    return fail("missing PE TLS directory");
  }

  struct image_tls_directory64 *tls =
      (void *)(image_base + opt->dirs[PE_DIR_TLS].va);
  if (tls->start_address_of_raw_data > tls->end_address_of_raw_data ||
      tls->address_of_callbacks != 0 ||
      !range_inside(image_base, image_bytes, tls->start_address_of_raw_data,
                    tls->end_address_of_raw_data -
                        tls->start_address_of_raw_data) ||
      !range_inside(image_base, image_bytes, tls->address_of_index,
                    sizeof(uint32_t))) {
    return fail("unsupported PE TLS directory");
  }

  uint32_t align_code = (tls->characteristics >> 20) & 0xFu;
  if (align_code == 15) {
    return fail("invalid PE TLS alignment");
  }
  uint64_t alignment = align_code == 0 ? 1 : 1ull << (align_code - 1);
  if (alignment > UPAGE_SIZE) {
    return fail("PE TLS alignment exceeds one page");
  }
  uint64_t raw_bytes =
      tls->end_address_of_raw_data - tls->start_address_of_raw_data;
  uint64_t tls_bytes;
  if (__builtin_add_overflow(raw_bytes, (uint64_t)tls->size_of_zero_fill,
                             &tls_bytes) ||
      tls_bytes > (1ull << 20)) {
    return fail("PE TLS template too large");
  }
  uint64_t data_offset = align_up(PE_TLS_DATA_MIN_OFFSET, alignment);
  uint64_t runtime_bytes;
  if (__builtin_add_overflow(data_offset, tls_bytes, &runtime_bytes)) {
    return fail("PE TLS runtime overflow");
  }
  uint64_t runtime =
      sys_vm_alloc(runtime_bytes, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(runtime)) {
    return fail("vm_alloc(TLS) failed");
  }

  uint64_t vector = runtime + PE_TLS_VECTOR_OFFSET;
  uint64_t data = runtime + data_offset;
  *(uint64_t *)(runtime + PE_TEB_SELF_OFFSET) = runtime;
  *(uint64_t *)(runtime + PE_TEB_TLS_VECTOR_OFFSET) = vector;
  *(uint64_t *)vector = data;
  *(uint32_t *)tls->address_of_index = 0;
  memcpy((void *)data, (const void *)tls->start_address_of_raw_data,
         raw_bytes);
  // VM_ALLOC already zeroed SizeOfZeroFill and all TEB/vector padding.
  return runtime;
}

uint64_t pe_spawn_resources(const uint8_t *image, uint64_t len, uint64_t arg,
                            uint64_t stack_len,
                            const struct pe_resource *resources,
                            uint32_t nresources) {
  if (len < sizeof(struct dos_header)) {
    return fail("image too short");
  }
  const struct dos_header *dos = (const void *)image;
  if (dos->e_magic != 0x5A4D /* MZ */ || dos->e_lfanew + 4 > len) {
    return fail("bad DOS header");
  }
  const struct coff_header *coff = (const void *)(image + dos->e_lfanew);
  if (coff->signature != 0x00004550 /* PE\0\0 */ || coff->machine != 0x8664) {
    return fail("bad PE signature/machine");
  }
  const struct optional_header64 *opt = (const void *)(coff + 1);
  if (opt->magic != 0x20B) {
    return fail("not PE32+");
  }
  if (opt->section_align != UPAGE_SIZE) {
    return fail("unsupported section alignment");
  }
  if (opt->n_data_dirs > PE_DIR_IMPORT && opt->dirs[PE_DIR_IMPORT].size != 0) {
    return fail("image has imports; static executables only");
  }
  if ((coff->characteristics & 0x0001 /* RELOCS_STRIPPED */) != 0) {
    return fail("image not relocatable (linked /fixed?)");
  }

  const struct section_header *sections =
      (const void *)((const uint8_t *)opt + coff->opt_size);
  uint64_t image_size = page_ceil(opt->size_of_image);

  // One block for the whole image, written here, moved to the embryo
  // below. VM_ALLOC zeroes, so the VirtualSize > raw_size tail (.bss) is
  // already right.
  uint64_t base = sys_vm_alloc(image_size, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(base)) {
    return fail("vm_alloc(image) failed");
  }
  memcpy((uint8_t *)base, image, opt->size_of_headers);
  for (uint16_t i = 0; i < coff->n_sections; i++) {
    const struct section_header *s = &sections[i];
    if ((uint64_t)s->virtual_address + s->raw_size > image_size ||
        (uint64_t)s->raw_offset + s->raw_size > len) {
      sys_vm_free(base);
      return fail("section out of bounds");
    }
    memcpy((uint8_t *)(base + s->virtual_address), image + s->raw_offset,
           s->raw_size);
  }

  // Base relocations against the block's address — which is also the
  // child's view of it (SASOS), so entry and every absolute pointer are
  // valid over there without any translation.
  uint64_t delta = base - opt->image_base;
  if (delta != 0 && opt->n_data_dirs > PE_DIR_BASERELOC &&
      opt->dirs[PE_DIR_BASERELOC].size != 0) {
    const uint8_t *rel =
        (const uint8_t *)(base + opt->dirs[PE_DIR_BASERELOC].va);
    const uint8_t *rel_end = rel + opt->dirs[PE_DIR_BASERELOC].size;
    while (rel + 8 <= rel_end) {
      uint32_t page_rva = *(const uint32_t *)rel;
      uint32_t block_size = *(const uint32_t *)(rel + 4);
      if (block_size < 8) {
        sys_vm_free(base);
        return fail("bad reloc block");
      }
      const uint16_t *entry = (const uint16_t *)(rel + 8);
      uint64_t n = (block_size - 8) / 2;
      for (uint64_t e = 0; e < n; e++) {
        uint16_t type = entry[e] >> 12;
        uint16_t off = entry[e] & 0xFFF;
        if (type == RELOC_DIR64) {
          *(uint64_t *)(base + page_rva + off) += delta;
        } else if (type != RELOC_ABSOLUTE) {
          sys_vm_free(base);
          return fail("unsupported reloc type");
        }
      }
      rel += block_size;
    }
  }

  uint64_t tls_runtime = pe_tls_create(base);
  if (tls_runtime == 0) {
    sys_vm_free(base);
    return 0;
  }

  // Embryo construction (§5): image and stack move down the tree edge,
  // then the parent sets the moved image's per-section W^X — its one
  // window of authority over another process's views.
  uint64_t pid = sys_proc_create();
  if (sys_iserr(pid)) {
    sys_vm_free(base);
    return fail("proc_create failed");
  }
  uint64_t stack_request;
  if (__builtin_add_overflow(stack_len, UPAGE_SIZE, &stack_request)) {
    sys_vm_free(tls_runtime);
    sys_vm_free(base);
    sys_proc_kill(pid);
    return fail("stack size overflow");
  }
  uint64_t stack =
      sys_vm_alloc(stack_request, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(stack)) {
    sys_vm_free(tls_runtime);
    sys_vm_free(base);
    sys_proc_kill(pid);
    return fail("vm_alloc(stack) failed");
  }
  uint64_t stack_bytes = sys_vm_size(stack);
  *(uint64_t *)(tls_runtime + 0x08) = stack + stack_bytes;
  *(uint64_t *)(tls_runtime + 0x10) = stack + UPAGE_SIZE;
  if (sys_iserr(sys_vm_move(base, pid)) ||
      sys_iserr(sys_vm_move(stack, pid)) ||
      sys_iserr(sys_vm_move(tls_runtime, pid))) {
    sys_proc_kill(pid);
    return fail("vm_move failed");
  }

  if (sys_iserr(sys_vm_protect_for(stack, UPAGE_SIZE, 0, pid))) {
    sys_proc_kill(pid);
    return fail("stack guard failed");
  }
  for (uint32_t i = 0; i < nresources; i++) {
    uint64_t rc = sys_vm_share(resources[i].base, pid, resources[i].prot);
    if (sys_iserr(rc)) {
      sys_proc_kill(pid);
      return fail("resource share failed");
    }
  }

  // Header page read-only, then each section per its characteristics.
  sys_vm_protect_for(base, UPAGE_SIZE, VM_PROT_READ, pid);
  for (uint16_t i = 0; i < coff->n_sections; i++) {
    const struct section_header *s = &sections[i];
    uint64_t prot = 0;
    if (s->characteristics & SEC_READ) {
      prot |= VM_PROT_READ;
    }
    if (s->characteristics & SEC_WRITE) {
      prot |= VM_PROT_WRITE;
    }
    if (s->characteristics & SEC_EXEC) {
      prot |= VM_PROT_EXEC;
    }
    uint64_t size = page_ceil(s->virtual_size ? s->virtual_size : s->raw_size);
    sys_vm_protect_for(base + s->virtual_address, size, prot, pid);
  }

  struct gdos_thread_start start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(start),
      .entry = base + opt->entry_point,
      .argument = arg,
      .stack_pointer =
          stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
      .gs_base = tls_runtime,
  };
  uint64_t tid = sys_thread_spawn(pid, &start);
  if (sys_iserr(tid)) {
    sys_proc_kill(pid);
    return fail("thread_spawn failed");
  }
  return pid;
}

uint64_t pe_spawn(const uint8_t *image, uint64_t len, uint64_t arg,
                  uint64_t stack_len) {
  return pe_spawn_resources(image, len, arg, stack_len, nullptr, 0);
}
