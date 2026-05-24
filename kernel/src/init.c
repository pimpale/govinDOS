#include <stdint.h>

#include "acpi.h"
#include "allocator.h"
#include "cpu_hwid.h"
#include "cpu_setup.h"
#include "debug.h"
#include "enumerate_cpus.h"
#include "get_mmap.h"
#include "interrupts.h"
#include "paging.h"
#include "scheduler.h"
#include "serial.h"
#include "smp.h"

#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "thread.h"

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

  // Spawn a single bootstrap thread. Round-robin places it on CPU 0
  // (the BSP, last to enter the scheduler); the other CPUs get nothing
  // and HLT. The spawner then enqueues 8 workers from inside its
  // running context, which round-robin onto all CPUs including the
  // ones currently HLT'd. Their wakeup proves the IPI path works.
  kthread_spawn(spawner_thread, (void *)8);

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
