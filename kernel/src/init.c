#include <stdint.h>

#include "acpi.h"
#include "allocator.h"
#include "capability.h"
#include "cpu_hwid.h"
#include "cpu_setup.h"
#include "debug.h"
#include "espfile.h"
#include "futex.h"
#include "get_mmap.h"
#include "iommu.h"
#include "paging.h"
#include "pe.h"
#include "platform_mem.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "slabs.h"
#include "smp.h"
#include "syscall.h"

#include "stdlib/stdio.h"
#include "thread.h"
#include "umem.h"

#include "boot/selftest.h"

#include <efi/efi.h>
#include <efi/graphics_output_protocol.h>
#include <efi/types.h>
#include <gdosabi/bootinfo.h>

#define AP_TRAMPOLINE_BASE 0x8000

// Everything only discoverable before exit_boot_services, captured in
// efi_main and handed to init as its bootinfo block (§3 of
// docs/technical/boot-init-design.md).
static struct bootinfo g_bootinfo;

static void bootinfo_capture(struct efi_system_table *system) {
  g_bootinfo.magic = BOOTINFO_MAGIC;
  g_bootinfo.version = BOOTINFO_VERSION;
  g_bootinfo.length = sizeof(g_bootinfo);
  g_bootinfo.fb_format = BOOTINFO_FB_NONE;

  struct efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  struct efi_graphics_output_protocol *gop = nullptr;
  efi_status_t status =
      system->boot->locate_protocol(&gop_guid, nullptr, (void **)&gop);
  if (status == EFI_SUCCESS && gop->mode != nullptr &&
      gop->mode->info != nullptr) {
    const struct efi_graphics_output_mode_information *mi = gop->mode->info;
    g_bootinfo.fb_base = gop->mode->frame_buffer_base;
    g_bootinfo.fb_size = gop->mode->frame_buffer_size;
    g_bootinfo.fb_width = mi->horizontal_resolution;
    g_bootinfo.fb_height = mi->vertical_resolution;
    g_bootinfo.fb_pixels_per_scanline = mi->pixels_per_scan_line;
    g_bootinfo.fb_format = mi->pixel_format;
  }
}

// init: the root of the process tree, loaded from \boot\init.exe on the
// ESP — the one image the kernel ever loads itself; all further
// processes are built by their parents in userspace. init's death is a
// panic (process.c), and so is its absence: there is no fallback init.
static void init_setup(const uint8_t *image, size_t image_len) {
  struct process *p = process_create(nullptr);
  process_set_init(p);
  capability_bootstrap(p, &g_bootinfo.cap_devmem, &g_bootinfo.cap_irq,
                       &g_bootinfo.cap_iommu);
  uint64_t entry = 0;
  uint64_t gs_base = 0;
  int rc = pe_load(p, image, image_len, &entry, &gs_base);
  asserts(rc == 0, "init: PE load failed");

  void *stack = umem_alloc(p, 5 * PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(stack != nullptr, "init: stack alloc failed");
  uint64_t stack_bytes = umem_size(p, (uint64_t)stack);
  *(uint64_t *)(gs_base + 0x08) = (uint64_t)stack + stack_bytes;
  // Bootstrap is the sole process with no userspace parent loader. Publish
  // the allocation's lower bound in NT_TIB.StackLimit; init's own startup
  // code chooses and installs its guard before doing real work.
  *(uint64_t *)(gs_base + 0x10) = (uint64_t)stack;

  // The bootinfo block: written through the kernel's own view, then the
  // one user view drops to read-only. Its base is init's entire entry
  // ABI (the arg lands in rcx, a plain first parameter to _start).
  struct bootinfo *bi = umem_alloc(p, PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(bi != nullptr, "init: bootinfo alloc failed");
  *bi = g_bootinfo;
  asserts(umem_protect(p, (uint64_t)bi, PAGE_SIZE, PAGE_R) == 0,
          "init: bootinfo protect failed");

  printf("init: pid=%llu entry=%016llX bootinfo=%016llX\n", p->pid, entry,
         (uint64_t)bi);
  struct gdos_thread_start start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(start),
      .entry = entry,
      .argument = (uint64_t)bi,
      .stack_pointer =
          (uint64_t)stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
      .gs_base = gs_base,
  };
  process_spawn_thread(p, &start);
}

[[noreturn]] static void ap_main() {
  cpu_setup();
  printf("ap[%llu]: hello from long mode\n", cpu_state_whoami());
  cpu_signal_alive();

  scheduler_loop(&g_cpu_state_table[cpu_state_whoami()].scheduler);
}

efi_status_t efi_main(efi_handle_t handle, struct efi_system_table *system) {
  serial_init();

  printf("starting kernel!\n");

  // Reserve the AP trampoline page at a fixed sub-1MiB physical address.
  // Pinning it through UEFI ensures (a) firmware isn't using it and (b) the
  // page comes back in the memory map as EFI_LOADER_DATA so kstdlib_init's
  // non-conventional-region pass marks it unusable in the buddy allocator.
  efi_physical_address_t trampoline_addr = AP_TRAMPOLINE_BASE;
  efi_status_t tramp_status = system->boot->allocate_pages(
      EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, 1, &trampoline_addr);
  bool have_trampoline = (tramp_status == EFI_SUCCESS);
  if (!have_trampoline) {
    printf("smp: could not reserve trampoline page at %016llX (status=%llu)\n",
           (uint64_t)AP_TRAMPOLINE_BASE, (uint64_t)tramp_status);
  }

  // Read init off the ESP and capture the GOP while boot services can
  // still do the work. Both allocate, so they must precede the final
  // get_memory_map — anything that changes the map invalidates mmap_key
  // and exit_boot_services would bounce.
  uint64_t init_image_len = 0;
  const uint8_t *init_image =
      esp_read_file(handle, system, (const efi_char16_t *)u"\\boot\\init.exe",
                    &init_image_len);
  asserts(init_image != nullptr, "init: \\boot\\init.exe missing from ESP");
  printf("init: read \\boot\\init.exe (%llu bytes)\n", init_image_len);
  bootinfo_capture(system);

  // get memory map
  efi_uint_t n_mmap = 0;
  struct efi_memory_descriptor *mmap = nullptr;
  efi_uint_t mmap_key = 0;
  efi_status_t mmap_status = get_memory_map(system, &mmap, &n_mmap, &mmap_key);
  asserts(mmap_status == EFI_SUCCESS, "failed to get memory map!\n");

  for (efi_uint_t i = 0; i < n_mmap; i++) {
    g_bootinfo.mem_total_pages += mmap[i].pages;
    if (mmap[i].type == EFI_CONVENTIONAL_MEMORY) {
      g_bootinfo.mem_usable_pages += mmap[i].pages;
    }
  }

  // grab anything that requires boot services / the system table before we
  // throw it all away
  const struct acpi_rsdp *rsdp = acpi_init(system);
  const struct acpi_madt *madt = acpi_find_madt(rsdp);
  g_bootinfo.acpi_rsdp = (uint64_t)rsdp;

  // exit boot services
  efi_status_t exit_status = system->boot->exit_boot_services(handle, mmap_key);
  if (exit_status != EFI_SUCCESS) {
    printf("failed to exit boot loader!\n");
    return exit_status;
  }

  // set up allocator
  allocator_init(n_mmap, mmap);

  // now we can initialize the cpu state table.
  cpu_state_table_init(madt);

  // Fixed-type allocators need the final CPU count before any slab-backed
  // container can allocate.
  slabs_init();

  // set up early boot processor stuff
  // allocates per-cpu kernel stacks for all cpus
  // paging, interrupts, etc still not yet set up
  cpu_setup_bsp(rsdp);

  // Snapshot the firmware classification before its loader-data map can be
  // recycled, then establish VT-d default-deny before any user AS exists.
  platform_mem_init(n_mmap, mmap, rsdp);
  iommu_init_required(rsdp);

  // Initialize per-CPU runqueues before bringing up APs — APs will call
  // scheduler_loop() as soon as they finish cpu_setup, which needs
  // cs->scheduler.lock + queue to be live.
  scheduler_init();

  // User-memory bookkeeping (ublock registry).
  umem_init();
  siphash_selftest();
  llrb_identity_selftest();
  // Futex bucket table — before the first user process can park.
  futex_init();
  capability_init();

  // Guard punch + revert must return the kernel tree to its exact shape.
  paging_merge_selftest();

  // Boot selftests. All per-CPU kernel stacks (and their guard punches)
  // exist by now, so g_as_kernel is in its final boot-static shape and
  // user ASes can clone it directly.
  umem_selftest();
  device_block_selftest(rsdp, g_bootinfo.fb_base);
  channel_selftest();
  process_selftest();

  // The root of the process tree. Everything else is spawned from
  // userspace, parents building children.
  init_setup(init_image, init_image_len);

  uint64_t bsp_id = cpu_hwid();

  for (size_t i = 0; i < g_cpu_state_table_len; i++) {
    if (g_cpu_state_table[i].hw_id == bsp_id) {
      continue;
    }
    printf("smp: starting cpu hw_id=%llu\n", g_cpu_state_table[i].hw_id);
    cpu_start(g_cpu_state_table[i].hw_id, ap_main,
              g_cpu_state_table[i].stacks.kernel_bootstrap_top);
  }

  // do the same stuff as any other processor on the system
  ap_main();
}
