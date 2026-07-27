#include <stdint.h>

#include "acpi.h"
#include "allocator.h"
#include "channel.h"
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
#include "smp.h"
#include "syscall.h"

#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "thread.h"
#include "uaccess.h"
#include "umem.h"

#include <efi/efi.h>
#include <efi/graphics_output_protocol.h>
#include <efi/types.h>
#include <gdosabi/bootinfo.h>

#define AP_TRAMPOLINE_BASE 0x8000

// Synchronous teardown for boot-test processes (no threads ever ran, so
// no culling or draining can be pending): kill the subtree, then run the
// reap loop to completion — which also regression-tests the reap steps
// themselves at every boot.
static void destroy_test_process(struct process *p) {
  process_kill_subtree(p);
  uint64_t rc;
  do {
    rc = process_reap_step(p);
    asserts(rc == REAP_MORE || rc == REAP_DONE,
            "destroy_test_process: reap stalled");
  } while (rc != REAP_DONE);
}

// Regression test for the paging merge pass: a guard punch fragments the
// kernel tree; reverting it must fold the tables back. Punch inside a
// 2 MiB buddy block: the buddy aligns blocks to their size, so that
// block's PD entry is untouched by anything else (boot-time stack guards
// live in other 2 MiB regions) and the punch is guaranteed to split at
// least the PT — the test can't pass vacuously.
static void paging_merge_selftest(void) {
  uint64_t baseline = as_table_count(g_as_kernel);
  uint8_t *blk = malloc(2 * 1024 * 1024);
  asserts(blk != nullptr, "merge selftest: alloc failed");
  uint64_t pg = (uint64_t)blk + PAGE_SIZE;
  as_flag(g_as_kernel, pg, pg + PAGE_SIZE, 0);
  as_flush(g_as_kernel);
  uint64_t split = as_table_count(g_as_kernel);
  asserts(split > baseline, "merge selftest: guard punch did not split");
  as_flag(g_as_kernel, pg, pg + PAGE_SIZE, PAGE_KERNEL_PRISTINE);
  as_flush(g_as_kernel);
  uint64_t merged = as_table_count(g_as_kernel);
  asserts(merged == baseline, "merge selftest: revert did not re-merge");
  free(blk);
  printf("paging: merge selftest ok (tables %llu -> %llu -> %llu)\n", baseline,
         split, merged);
}

// Boot-time test of the ublock model across two processes: isolation
// (PAGE_U only in the owner's tree), sharing, per-view protect, and
// revoke-on-free. Runs before the scheduler ever dispatches anything, so
// the reap loop in destroy_test_process finishes without retries.
static void umem_selftest(void) {
  struct process *a = process_create(nullptr);
  struct process *b = process_create(nullptr);

  uint8_t *blk = umem_alloc(a, 2 * PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(blk != nullptr, "umem selftest: alloc failed");
  asserts(umem_size(a, (uint64_t)blk) == 2 * PAGE_SIZE,
          "umem selftest: size query failed");
  asserts(umem_size(a, (uint64_t)blk + PAGE_SIZE) == 0 &&
              umem_size(b, (uint64_t)blk) == 0,
          "umem selftest: size query accepted non-owner/base");
  asserts(blk[0] == 0 && blk[2 * PAGE_SIZE - 1] == 0,
          "umem selftest: block not zeroed");

  // Isolation: PAGE_U in the owner's tree only; everywhere else the block
  // is plain kernel memory.
  asserts(user_range_ok(a, (uint64_t)blk, 2 * PAGE_SIZE, true),
          "umem selftest: owner cannot access own block");
  asserts(!user_range_ok(b, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: stranger can access foreign block");

  // Sharing: read-only view for b.
  asserts(umem_share(a, (uint64_t)blk, b->pid, PAGE_R) == 0,
          "umem selftest: share failed");
  asserts(user_range_ok(b, (uint64_t)blk, 2 * PAGE_SIZE, false),
          "umem selftest: sharer cannot read shared block");
  asserts(!user_range_ok(b, (uint64_t)blk, PAGE_SIZE, true),
          "umem selftest: read-only sharer can write");

  // Per-view flags: owner guards a sub-range; sharer's view unaffected.
  asserts(umem_protect(a, (uint64_t)blk, PAGE_SIZE, 0) == 0,
          "umem selftest: protect failed");
  asserts(!user_range_ok(a, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: owner guard view not applied");
  asserts(user_range_ok(b, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: owner's protect leaked into sharer view");

  // The single-transaction free refuses while the edge remains; the
  // owner's per-edge revoke (the teardown coercion path) drains it, then
  // the free succeeds and restores pristine everywhere.
  asserts(umem_free(a, (uint64_t)blk) == (int)SYSERR_EXIST,
          "umem selftest: free ignored attached sharer");
  asserts(umem_unshare_from(a, (uint64_t)blk, b->pid) == 0,
          "umem selftest: unshare failed");
  asserts(!user_range_ok(b, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: unshare left sharer access");
  asserts(umem_free(a, (uint64_t)blk) == 0, "umem selftest: free failed");
  asserts(!user_range_ok(a, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: free left owner access");

  // Full-block restore + merge: both trees back to the kernel skeleton's
  // shape (user ASes are clones of boot-static g_as_kernel).
  uint64_t skel = as_table_count(g_as_kernel);
  asserts(as_table_count(a->as) == skel && as_table_count(b->as) == skel,
          "umem selftest: trees did not merge back to skeleton shape");

  destroy_test_process(a);
  destroy_test_process(b);
  printf("umem: selftest ok\n");
}

static void device_block_selftest(const struct acpi_rsdp *rsdp,
                                  uint64_t framebuffer_base) {
  struct process *a = process_create(nullptr);
  struct process *b = process_create(nullptr);

  // Usable RAM is never accepted as device memory.
  uint8_t *ram = umem_alloc(a, PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(ram != nullptr, "devmem selftest: RAM alloc failed");
  asserts(umem_map_device(a, (uint64_t)ram, PAGE_SIZE,
                          VM_DEVICE_READ | VM_DEVICE_WRITE) == SYSERR_INVAL,
          "devmem selftest: conventional RAM accepted");
  asserts(umem_free(a, (uint64_t)ram) == 0,
          "devmem selftest: RAM cleanup failed");

  // Firmware backing is readable but never delegatable.
  uint64_t fw = (uint64_t)rsdp & ~(uint64_t)(PAGE_SIZE - 1);
  asserts(umem_map_device(a, fw, PAGE_SIZE,
                          VM_DEVICE_READ | VM_DEVICE_FIRMWARE) == 0,
          "devmem selftest: ACPI map failed");
  asserts(user_range_ok(a, fw, PAGE_SIZE, false),
          "devmem selftest: ACPI owner cannot read");
  asserts(umem_share(a, fw, b->pid, PAGE_R) == SYSERR_INVAL,
          "devmem selftest: ACPI block delegated");
  asserts(umem_free(a, fw) == 0, "devmem selftest: ACPI free failed");

  // A Q35 framebuffer page exercises the ordinary UC, delegatable case.
  if (framebuffer_base >= 0xC0000000ull &&
      framebuffer_base + PAGE_SIZE <= 0xFEC00000ull) {
    uint64_t mmio = framebuffer_base & ~(uint64_t)(PAGE_SIZE - 1);
    asserts(umem_map_device(a, mmio, PAGE_SIZE,
                            VM_DEVICE_READ | VM_DEVICE_WRITE) == 0,
            "devmem selftest: MMIO map failed");
    asserts(umem_map_device(b, mmio, PAGE_SIZE, VM_DEVICE_READ) == SYSERR_EXIST,
            "devmem selftest: overlapping device block allowed");
    asserts(umem_share(a, mmio, b->pid, PAGE_R) == 0,
            "devmem selftest: BAR-style share failed");
    paging_flags_t f;
    bool present;
    asserts(as_getinfo(b->as, mmio, &f, &present) == 0 && present &&
                (f & (PAGE_U | PAGE_UC)) == (PAGE_U | PAGE_UC) && !(f & PAGE_W),
            "devmem selftest: shared cache/rights not inherited");
    asserts(umem_dropshare(b, mmio) == 0,
            "devmem selftest: BAR-style dropshare failed");
    asserts(umem_free(a, mmio) == 0, "devmem selftest: device free failed");
  }

  destroy_test_process(a);
  destroy_test_process(b);
  printf("devmem: device-backed ublock selftest ok\n");
}

// Boot-time test of the channel plumbing that doesn't need running
// threads: shares-channel creation, KEV_SHARE posting (both the
// immediate path and the replay of edges that predate the channel), and
// endpoint teardown through process death.
static void channel_selftest(void) {
  struct process *a = process_create(nullptr); // sharer
  struct process *b = process_create(nullptr); // shares-channel owner

  // Share BEFORE b has a shares channel: the edge holds the pending
  // notification (level state), to be replayed at channel creation.
  uint8_t *early = umem_alloc(a, PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(early != nullptr, "channel selftest: alloc failed");
  asserts(umem_share(a, (uint64_t)early, b->pid, PAGE_R) == 0,
          "channel selftest: share failed");

  uint8_t *ch = umem_alloc(b, PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(ch != nullptr, "channel selftest: channel alloc failed");
  asserts(channel_scheme_create(b, (uint64_t)ch, KSCHEME_SHARES) == 0,
          "channel selftest: scheme create failed");

  volatile struct kring_hdr *h = (volatile struct kring_hdr *)ch;
  const struct kcqe *cq =
      (const struct kcqe *)(ch + KRING_HDR_SIZE +
                            KRING_NSLOTS(0) * sizeof(struct ksqe));
  asserts(h->nslots == KRING_NSLOTS(0), "channel selftest: bad nslots");
  asserts(h->cq_count == 1, "channel selftest: replay did not post");
  asserts(cq[0].type == KEV_SHARE && cq[0].a == a->pid &&
              cq[0].b == ((uint64_t)early | 0),
          "channel selftest: bad replayed KEV_SHARE");

  // Share with the channel live: immediate notification.
  uint8_t *late = umem_alloc(a, 2 * PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(late != nullptr, "channel selftest: alloc failed");
  asserts(umem_share(a, (uint64_t)late, b->pid, PAGE_R | PAGE_W) == 0,
          "channel selftest: live share failed");
  asserts(h->cq_count == 2, "channel selftest: live share did not post");
  asserts(cq[1].type == KEV_SHARE && cq[1].a == a->pid &&
              cq[1].b == ((uint64_t)late | 1),
          "channel selftest: bad live KEV_SHARE");

  // Rules: one shares channel per process; no scheme on a shared block.
  uint8_t *spare = umem_alloc(b, PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(channel_scheme_create(b, (uint64_t)spare, KSCHEME_SHARES) ==
              SYSERR_EXIST,
          "channel selftest: second shares channel allowed");
  asserts(channel_scheme_create(a, (uint64_t)early, KSCHEME_SHARES) ==
              SYSERR_INVAL,
          "channel selftest: scheme on a shared block allowed");
  // IRQ rings stay mapped because the hardware handler can publish from a
  // borrowed context. Creation policy moves to capabilities later.
  asserts(channel_scheme_create(b, (uint64_t)spare, KSCHEME_IRQ) == 0,
          "channel selftest: IRQ ring create failed");
  asserts(umem_protect(b, (uint64_t)spare, PAGE_SIZE, PAGE_R) != 0,
          "channel selftest: IRQ ring protect allowed");

  // Teardown through the ordinary death paths: a's blocks revoke out of
  // b's AS, b's channel block dies with its ring endpoint.
  destroy_test_process(a);
  destroy_test_process(b);
  printf("channel: selftest ok\n");
}

// Boot-time test of the tree machinery that doesn't need running
// threads: embryo creation, VM_MOVE down (construction) and up
// (reap-time claim), the tree channel's KEV_CHILD_DEAD (replay path),
// and the subtree reap cursor.
static void process_selftest(void) {
  struct process *parent = process_create(nullptr);
  struct process *child = process_create(parent);
  struct process *grandchild = process_create(child);
  asserts(child->state == PROC_EMBRYO, "process selftest: not an embryo");

  // Move a block down into the embryo: parent view gone, child view RW.
  uint8_t *blk = umem_alloc(parent, PAGE_SIZE, PAGE_R | PAGE_W);
  blk[42] = 0x42; // survives the move (same physical identity address)
  umem_lock();
  umem_proc_lock(parent);
  ublock *b = umem_owned_locked(parent, (uint64_t)blk);
  umem_proc_unlock(parent);
  asserts(b != nullptr && umem_move_locked(b, parent, child, true) == 0,
          "process selftest: move down failed");
  umem_unlock();
  asserts(!user_range_ok(parent, (uint64_t)blk, PAGE_SIZE, false),
          "process selftest: mover kept a view");
  asserts(user_range_ok(child, (uint64_t)blk, PAGE_SIZE, true),
          "process selftest: movee got no view");
  asserts(blk[42] == 0x42, "process selftest: move lost contents");

  // Tree channel + death: create the channel first, then kill the child
  // — the event must post immediately (the replay path is exercised by
  // the death being level state until consumed).
  uint8_t *tch = umem_alloc(parent, PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(channel_scheme_create(parent, (uint64_t)tch, KSCHEME_TREE) == 0,
          "process selftest: tree channel create failed");
  process_kill_subtree(child);
  // Only the subtree root is materialized synchronously. Its descendant
  // is immediately dead by ancestry, hidden from live-pid lookup, and is
  // materialized later by bounded post-order reap.
  asserts(grandchild->state == PROC_EMBRYO && process_is_dead(grandchild),
          "process selftest: descendant death was not lazy");
  umem_lock();
  asserts(umem_proc_lookup_locked(grandchild->pid) == nullptr,
          "process selftest: effective-dead pid remained reachable");
  umem_unlock();
  volatile struct kring_hdr *h = (volatile struct kring_hdr *)tch;
  const struct kcqe *cq =
      (const struct kcqe *)(tch + KRING_HDR_SIZE +
                            KRING_NSLOTS(0) * sizeof(struct ksqe));
  asserts(h->cq_count == 1 && cq[0].type == KEV_CHILD_DEAD &&
              cq[0].a == child->pid,
          "process selftest: no KEV_CHILD_DEAD");

  // Claim the block back up out of the zombie before reaping frees it.
  umem_lock();
  umem_proc_lock(child);
  b = umem_owned_locked(child, (uint64_t)blk);
  umem_proc_unlock(child);
  asserts(b != nullptr && umem_move_locked(b, child, parent, true) == 0,
          "process selftest: move up failed");
  umem_unlock();
  asserts(user_range_ok(parent, (uint64_t)blk, PAGE_SIZE, true),
          "process selftest: claim got no view");
  asserts(blk[42] == 0x42, "process selftest: claim lost contents");

  // Reap the zombie child, then tear down the parent (whose subtree is
  // now just itself).
  uint64_t rc;
  do {
    rc = process_reap_step(child);
    asserts(rc == REAP_MORE || rc == REAP_DONE,
            "process selftest: reap stalled");
  } while (rc != REAP_DONE);
  asserts(umem_free(parent, (uint64_t)blk) == 0,
          "process selftest: claimed block not owned");
  destroy_test_process(parent);
  printf("process: selftest ok\n");
}

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
  capability_selftest(p, &g_bootinfo.cap_devmem);
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
