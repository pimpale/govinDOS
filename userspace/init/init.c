// init for govindos: the root of the process tree, the one image the
// kernel ever loads (from \boot\init.exe on the ESP; see
// docs/technical/boot-init-design.md). Its job is deliberately small:
// verify the bootinfo handoff, pull children out of the initfs (the
// cpio archive linked into this binary as the .bootfs section), spawn
// them the parent-driven way (upe.c), and reap what dies. init must
// never exit — that's a kernel panic.
//
// Freestanding: no libc, no imports (the loader rejects import tables).
// Built with -mgeneral-regs-only — the kernel preserves x87/SSE across
// switches but not AVX, so YMM+ state must stay unused.

#include <stdint.h>

#include <gdos/bootinfo.h>

#include "cpio.h"
#include "upe.h"
#include "usys.h"

// The initfs bounds, provided by bootfs.asm (incbin of initdata.cpio).
// Discovery is two linked symbols — no PE self-parsing, no bootinfo
// entry (boot-init-design.md §0).
extern const uint8_t bootfs_start[];
extern const uint8_t bootfs_end[];

// Absolute address baked into .data at link time: forces a DIR64 base
// relocation that rip-relative access can't fold away (volatile). If the
// kernel's boot loader rebases wrong, this jumps into the weeds instead
// of printing. (tests.exe carries the same probe for the userland
// loader.)
typedef void (*printer_t)(const char *);
static void print_probe(const char *s) { print(s); }
static volatile printer_t g_reloc_fn = print_probe;
static const char g_reloc_probe[] = "init: relocated fn ptr works\n";
static const char *volatile g_reloc_str = g_reloc_probe;

static void bootinfo_report(const struct bootinfo *bi) {
  if (bi == nullptr || bi->magic != BOOTINFO_MAGIC ||
      bi->version != BOOTINFO_VERSION) {
    print("init: BOOTINFO MISSING OR BAD\n");
    return;
  }
  print("init: bootinfo magic ok\n");
  print("init: bootinfo rsdp=");
  print_hex(bi->acpi_rsdp);
  print("init: bootinfo fb=");
  print_hex(bi->fb_base);
  print("init: bootinfo usable pages=");
  print_hex(bi->mem_usable_pages);
}

// Drive SYS_PROC_REAP to completion, yielding through SYSERR_AGAIN
// (culling/drain may lag the death by a few dispatches).
static void reap_child(uint64_t pid) {
  while (1) {
    uint64_t rc = sys1(SYS_PROC_REAP, pid);
    if (rc == REAP_DONE) {
      return;
    }
    if (rc == SYSERR_AGAIN) {
      sys0(SYS_YIELD);
      continue;
    }
    if (rc != REAP_MORE) {
      print("init: REAP FAILED rc=");
      print_hex(rc);
      return;
    }
  }
}

// Wait for the next KEV_CHILD_DEAD on the tree channel and consume it.
static void await_child_death(uint64_t tch, uint32_t *seen) {
  volatile uint32_t *cq_count = (volatile uint32_t *)(tch + KRING_CQ_COUNT_OFF);
  volatile uint32_t *cq_head = (volatile uint32_t *)(tch + KRING_CQ_HEAD_OFF);
  volatile struct kcqe *cq =
      (volatile struct kcqe *)(tch + KRING_HDR_SIZE + 32 * 32);
  sys2(SYS_BLOCK_WAIT, (uint64_t)cq_count, *seen);
  while (__atomic_load_n(cq_count, __ATOMIC_ACQUIRE) == *seen) {
    sys0(SYS_YIELD);
  }
  uint64_t ok = (cq[*seen & 31].type & 0xFF) == KEV_LO(KEV_CHILD_DEAD);
  print(ok ? "init: KEV_CHILD_DEAD pid=" : "init: BAD TREE EVENT pid=");
  print_hex(cq[*seen & 31].a);
  (*seen)++;
  __atomic_store_n(cq_head, *seen, __ATOMIC_RELEASE);
  sys1(SYS_BLOCK_DOORBELL, tch); // consumption ack
}

void _start(uint64_t arg) {
  print("init: hello from the root of the process tree!\n");
  g_reloc_fn(g_reloc_str);
  bootinfo_report((const struct bootinfo *)arg);

  print("init: initfs bytes=");
  print_hex((uint64_t)(bootfs_end - bootfs_start));

  // Tree channel: children's deaths arrive here, and it doubles as
  // init's forever-park spot at the end (a kernel channel is always
  // waitable — no peer needs to stay alive).
  uint64_t tch = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("init: vm_share(tch, -3) rc=");
  print_hex(sys3(SYS_VM_SHARE, tch, KSCHEME_TREE, 0));
  uint32_t tree_seen = 0;

  // The ring-3 test suite, shipped in the initfs and built as a real
  // separate process by the userland loader — the first consumer of the
  // whole boot design.
  const uint8_t *tests = nullptr;
  uint64_t tests_len = 0;
  if (cpio_find(bootfs_start, (uint64_t)(bootfs_end - bootfs_start),
                "tests.exe", &tests, &tests_len) != 0) {
    print("init: INITFS MISSING tests.exe\n");
  } else {
    print("init: initfs tests.exe bytes=");
    print_hex(tests_len);
    uint64_t pid = upe_spawn(tests, tests_len, 0, 4 * 4096);
    print("init: spawned tests.exe pid=");
    print_hex(pid);
    if (pid != 0) {
      await_child_death(tch, &tree_seen);
      reap_child(pid);
      print("init: tests.exe reaped\n");
    }
  }

  print("init: up (parking on the tree channel)\n");
  volatile uint32_t *tree_count =
      (volatile uint32_t *)(tch + KRING_CQ_COUNT_OFF);
  while (1) {
    sys2(SYS_BLOCK_WAIT, (uint64_t)tree_count, tree_seen);
    sys0(SYS_YIELD);
  }
}
