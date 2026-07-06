#include <stdint.h>

#include "acpi.h"
#include "allocator.h"
#include "cpu_hwid.h"
#include "cpu_setup.h"
#include "debug.h"
#include "dummydev.h"
#include "enumerate_cpus.h"
#include "get_mmap.h"
#include "interrupts.h"
#include "paging.h"
#include "pe.h"
#include "scheduler.h"
#include "serial.h"
#include "smp.h"

#include "reaper.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "thread.h"
#include "uaccess.h"
#include "umem.h"

#include <efi/efi.h>
#include <efi/types.h>

#define AP_TRAMPOLINE_BASE 0x8000

// Smoke test for the scheduler: prints a few times, yielding between
// prints so other threads (and the idle loop) get to run.
static void hello_thread(void *arg) {
  uint64_t n = (uint64_t)arg;
  for (uint64_t i = 0; i < 5; i++) {
    printf("thread %llu tick %llu (cpu %llu)\n", n, i, cpu_state_whoami());
    yield();
  }
}

// IPI-wakeup test: spawns N hello_threads from inside a running thread.
// The expected sequence is that this thread runs on whichever CPU got it
// (round-robin starts at 0), while the other CPUs have already entered
// their scheduler loops, found their queues empty, and HLT'd. The
// spawned threads round-robin onto all CPUs including the idle ones —
// proving the wakeup IPI works if those CPUs actually run them.
static void spawner_thread(void *arg) {
  uint64_t n = (uint64_t)arg;
  printf("spawner on cpu %llu about to spawn %llu workers\n",
         cpu_state_whoami(), n);
  for (uint64_t i = 0; i < n; i++) {
    kthread_spawn(hello_thread, (void *)i);
  }
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
// revoke-on-free. Runs before the scheduler ever dispatches these
// processes, so process_destroy's drain is trivially satisfied.
static void umem_selftest(void) {
  struct process *a = process_create_user(9001);
  struct process *b = process_create_user(9002);

  uint8_t *blk = umem_alloc(a, 2 * PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(blk != nullptr, "umem selftest: alloc failed");
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

  // Owner free revokes the sharer and restores pristine everywhere.
  asserts(umem_free(a, (uint64_t)blk, 0) == 0, "umem selftest: free failed");
  asserts(!user_range_ok(b, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: revoke left sharer access");
  asserts(!user_range_ok(a, (uint64_t)blk, PAGE_SIZE, false),
          "umem selftest: free left owner access");

  // Full-block restore + merge: both trees back to template shape.
  uint64_t tmpl = as_table_count(g_as_template);
  asserts(as_table_count(a->as) == tmpl && as_table_count(b->as) == tmpl,
          "umem selftest: trees did not merge back to template shape");

  process_destroy(a);
  process_destroy(b);
  printf("umem: selftest ok\n");
}

// C userspace test program compiled to PE32+ (userspace/hello.c),
// embedded by user_pe_blob.asm until a filesystem exists.
extern uint8_t user_pe_blob[];
extern uint8_t user_pe_blob_end[];

static void pe_test_setup(void) {
  struct process *p = process_create_user(1001);
  uint64_t entry = 0;
  int rc = pe_load(p, user_pe_blob, (size_t)(user_pe_blob_end - user_pe_blob),
                   &entry);
  asserts(rc == 0, "pe_test: load failed");

  void *stack = umem_alloc(p, 4 * PAGE_SIZE, PAGE_R | PAGE_W);
  asserts(stack != nullptr, "pe_test: stack alloc failed");

  printf("pe_test: pid=%llu uid=%llu entry=%016llX\n", p->pid, p->uid, entry);
  uthread_spawn(p, entry, (uint64_t)stack + 4 * PAGE_SIZE);
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

  // get memory map
  efi_uint_t n_mmap = 0;
  struct efi_memory_descriptor *mmap = nullptr;
  efi_uint_t mmap_key = 0;
  efi_status_t mmap_status = get_memory_map(system, &mmap, &n_mmap, &mmap_key);
  asserts(mmap_status == EFI_SUCCESS, "failed to get memory map!\n");

  // grab anything that requires boot services / the system table before we
  // throw it all away
  const struct acpi_rsdp *rsdp = acpi_init(system);
  const struct acpi_madt *madt = acpi_find_madt(rsdp);

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

  // Initialize per-CPU runqueues before bringing up APs — APs will call
  // threading_cpu_enter() as soon as they finish cpu_setup, which needs
  // cs->scheduler.lock + queue to be live.
  scheduler_init();

  // Initialize threading before bringing up APs — APs will call
  // threading_cpu_enter() as soon as they finish cpu_setup, which
  // requires g_kernel_process to exist.
  threading_init();

  // User-memory bookkeeping (ublock registry + uid accounts).
  umem_init();

  // Guard punch + revert must return the kernel tree to its exact shape.
  paging_merge_selftest();

  // Seal the template BEFORE the first kthread_spawn: user ASes clone
  // from this frozen snapshot, which must contain the boot-static kernel
  // mappings (incl. per-CPU stack guards) but never a kthread-stack guard
  // — those are punched and restored in g_as_kernel only.
  g_as_template = as_clone(g_as_kernel);

  // Two-process isolation / share / revoke test (needs the template).
  umem_selftest();

  // Reaper before anything can die: dead threads (and, via the last
  // thread, dead processes) are torn down on its kthread.
  reaper_init();

  // Spawn a single bootstrap thread. Round-robin places it on CPU 0
  // (the BSP, last to enter the scheduler); the other CPUs get nothing
  // and HLT. The spawner then enqueues 8 workers from inside its
  // running context, which round-robin onto all CPUs including the
  // ones currently HLT'd. Their wakeup proves the IPI path works.
  kthread_spawn(spawner_thread, (void *)8);

  // Fake blocking device (spawns its producer kthread).
  dummydev_init();

  // Ring-3 test suite: a real C program (userspace/hello.c), compiled to
  // PE32+, rebased at load time. Exercises the whole syscall surface.
  pe_test_setup();

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
