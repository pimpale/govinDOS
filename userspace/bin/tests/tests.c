// The ring-3 test suite for govindos, shipped in the initfs and spawned
// by init through the userland PE loader (lib/sys pe.c) as a real
// separate process — which makes every boot regression-test that loader
// too. Descended from the original hello.c, which doubled as init and
// the suite until the boot-init design split the roles
// (docs/technical/boot-init-design.md §0).
//
// Children below are built here, parent-driven, the way real userspace
// does it (ipc-process-design.md §5): PROC_CREATE an embryo, VM_MOVE it
// a stack, VM_SHARE it this very image read-execute (SASOS: the child
// runs the same code at the same addresses), pre-seed a bootstrap
// channel, THREAD_SPAWN its first thread. Children exercise the channel
// data plane from the far side; kill/reap/tree-events are exercised from
// this side.
//
// Kernel channels go through lib/sys <kring.h> (so every boot exercises
// that library too); the bootstrap block stays hand-rolled — it's a
// *user* channel, its layout is our own convention, not kernel ABI.
//
// Freestanding: no libc, no imports (the loader rejects import tables).
// Built with -mgeneral-regs-only — the kernel preserves x87/SSE across
// switches (fxsave at park, fxrstor at resume) but not AVX, so YMM+
// state must stay unused. Child code must not write globals: its view
// of the image is read-execute (per-view W^X), so it works in stack
// locals only.

#include <stdint.h>

#include <gdos/kring_shares.h>
#include <gdos/kring_tree.h>

#include <kring.h>
#include <sys.h>

// Absolute addresses baked into .data at link time: these force DIR64
// base relocations that the compiler cannot fold into rip-relative
// accesses (volatile). If the loader rebases wrong, this jumps into the
// weeds instead of printing.
typedef void (*printer_t)(const char *);
static volatile printer_t g_reloc_fn = print;
static const char g_reloc_probe[] = "pe: relocated fn ptr + string work\n";
static const char *volatile g_reloc_str = g_reloc_probe;

static void test_memory(void) {
  // Allocate two pages RW, print through them, free.
  uint64_t base = sys_vm_alloc(8192, VM_PROT_READ | VM_PROT_WRITE);
  print("pe: vm_alloc base=");
  print_hex(base);

  const char *msg = "pe: printing from vm_alloc'd page\n";
  const char *msg2 = "pe: printing from a read-only page\n";
  char *pg = (char *)base;
  char *pg2 = (char *)(base + 4096);
  uint64_t len = strlen(msg);
  uint64_t len2 = strlen(msg2);
  for (uint64_t i = 0; i < len; i++) {
    pg[i] = msg[i];
  }
  for (uint64_t i = 0; i < len2; i++) {
    pg2[i] = msg2[i]; // seeded while still RW; read back after RO below
  }
  sys_debug_write(pg, len);

  // vm_protect: drop the second page to read-only in our own view.
  // Reads (debug_write) still work through it.
  print("pe: vm_protect(page2, RO) rc=");
  print_hex(sys_vm_protect(base + 4096, 4096, VM_PROT_READ));
  sys_debug_write(pg2, len2);

  // Guard view (prot=0): the kernel must now refuse even reads there.
  print("pe: vm_protect(page2, none) rc=");
  print_hex(sys_vm_protect(base + 4096, 4096, 0));
  print("pe: debug_write through guarded page rc=");
  print_hex(sys_debug_write(pg2, 16));

  // Restore and free. Blocks are the unit and the base names the block:
  // freeing a mid-block address must be rejected, the base frees it all.
  print("pe: vm_protect(page2, RW) rc=");
  print_hex(sys_vm_protect(base + 4096, 4096, VM_PROT_READ | VM_PROT_WRITE));
  print("pe: mid-block vm_free rc=");
  print_hex(sys_vm_free(base + 4096));
  print("pe: vm_free rc=");
  print_hex(sys_vm_free(base));

  // Double free must fail.
  print("pe: double vm_free rc=");
  print_hex(sys_vm_free(base));

  // Share error paths.
  uint64_t blk = sys_vm_alloc(4096, VM_PROT_READ);
  print("pe: vm_share to bogus pid rc=");
  print_hex(sys_vm_share(blk, 0xdead, VM_PROT_READ));
  print("pe: vm_share to self rc=");
  print_hex(sys_vm_share(blk, (int64_t)sys_getpid(), VM_PROT_READ));
  print("pe: vm_unshare of owned block rc=");
  print_hex(sys_vm_unshare(blk));
  print("pe: cleanup vm_free rc=");
  print_hex(sys_vm_free(blk));

  // The removed wait-group id stays vacant; later schemes retain their ids.
  struct kring removed;
  print("pe: removed scheme -2 rc=");
  print_hex(kring_create(&removed, (int64_t)-2, 4096));

  // The kernel must refuse to touch non-PAGE_U memory on our behalf
  // (expect SYSERR_FAULT, ...fffe).
  print("pe: kernel-ptr debug_write rc=");
  print_hex(sys_debug_write((const void *)0x1000, 16));
}

// ---------------------------------------------------------------------------
// Channel protocol on the bootstrap block (userspace convention): two
// 32-bit doorbell words, then a message buffer. init bumps `req`; the
// child answers in place, bumps `resp`.
// ---------------------------------------------------------------------------

#define CH_REQ_OFF 0
#define CH_RESP_OFF 4
#define CH_MSG_OFF 8
#define CH_MSG_MAX 64

// First thread of the served child. Runs on init's image (read-execute
// view — stack locals only!) with the bootstrap-channel base as its
// argument. Proves the whole far side of the establishment flow: the
// shares channel replays the pre-spawn seed share, the data plane works
// both ways, and revocation wakes a parked waiter with SYSERR_DEAD.
static void child_main(uint64_t boot_ch) {
  print("child: hello (spawned by tests)\n");

  // Create a shares channel; the bootstrap block was shared to us while
  // we were still an embryo, so its KEV_SHARE must replay right here
  // (as must the image's — both edges predate the ring).
  struct kring sch;
  print("child: kring_create(ch, -1) rc=");
  print_hex(kring_create(&sch, KSCHEME_SHARES, 4096));
  uint64_t found = 0;
  struct kcqe cqe;
  kring_wait_cqe(&sch, &cqe);
  while (1) {
    if (cqe.type == KEV_SHARE && (cqe.b & ~0xFFFull) == boot_ch) {
      found = 1;
    }
    const struct kcqe *next = kring_peek_cqe(&sch);
    if (next == nullptr) {
      break;
    }
    cqe = *next;
    kring_cqe_seen(&sch);
  }
  kring_ack(&sch);
  print(found ? "child: bootstrap share replayed ok\n"
              : "child: BOOTSTRAP SHARE MISSING\n");

  // Serve one request on the bootstrap block (we are the sharer side).
  volatile uint32_t *req = (volatile uint32_t *)(boot_ch + CH_REQ_OFF);
  volatile uint32_t *resp = (volatile uint32_t *)(boot_ch + CH_RESP_OFF);
  volatile char *msg = (volatile char *)(boot_ch + CH_MSG_OFF);
  sys_block_wait(req, 0);
  while (__atomic_load_n(req, __ATOMIC_ACQUIRE) == 0) {
    sys_yield();
  }
  for (uint64_t i = 0; i < CH_MSG_MAX && msg[i] != '\0'; i++) {
    if (msg[i] >= 'a' && msg[i] <= 'z') {
      msg[i] = (char)(msg[i] - 'a' + 'A');
    }
  }
  __atomic_store_n(resp, 1, __ATOMIC_RELEASE);
  sys_block_doorbell(boot_ch);

  // Park until init frees the bootstrap block: the revoke path must wake
  // us with SYSERR_DEAD. Don't touch the block afterwards.
  uint64_t rc = sys_block_wait(req, 1);
  print(rc == SYSERR_DEAD ? "child: revoke wake ok, exiting\n"
                          : "child: REVOKE WAKE WRONG\n");
  sys_exit();
}

// First thread of the kill-test child: spins in yield until killed.
static void child_spin_main(uint64_t arg) {
  (void)arg;
  print("victim: spinning until killed\n");
  while (1) {
    sys_yield();
  }
}

// First thread of the preemption-test child: burns CPU in ring 3 and
// never enters the kernel again. Only the quantum timer can pull it in,
// so killing AND reaping it proves preemption end to end — the reap
// needs the thread culled at a kernel entry and the AS drained off its
// CPU, neither of which can happen while it sits in ring 3.
static void child_burn_main(uint64_t arg) {
  (void)arg;
  print("burner: spinning in ring 3, no more syscalls\n");
  volatile uint64_t sink = 0;
  while (1) {
    sink++;
  }
}

// Build a child process the parent-driven way and return its pid.
// entry runs on our shared image; boot_ch (may be 0) is its argument.
static uint64_t spawn_child(void (*entry)(uint64_t), uint64_t boot_ch) {
  uint64_t pid = sys_proc_create();
  print("tests: proc_create pid=");
  print_hex(pid);

  // Stack: built here, ownership transferred into the embryo.
  uint64_t stack = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  print("tests: vm_move(stack) rc=");
  print_hex(sys_vm_move(stack, pid));
  // The mover keeps no view: reading through it must now fail.
  print("tests: debug_write through moved stack rc=");
  print_hex(sys_debug_write((const void *)stack, 8));

  // Code: share our own image read-execute. SASOS means the child sees
  // it at the same address, so `entry` is valid over there too.
  print("tests: vm_share(image, RX) rc=");
  print_hex(sys_vm_share((uint64_t)&__ImageBase, (int64_t)pid,
                         VM_PROT_READ | VM_PROT_EXEC));

  if (boot_ch != 0) {
    // Pre-seed the bootstrap channel while the child is an embryo — the
    // seL4/Xen answer to how strangers ever get introduced.
    print("tests: vm_share(boot_ch) rc=");
    print_hex(
        sys_vm_share(boot_ch, (int64_t)pid, VM_PROT_READ | VM_PROT_WRITE));
  }

  uint64_t tid = sys_thread_spawn(pid, (uint64_t)entry, stack + 4096, boot_ch);
  print("tests: thread_spawn tid=");
  print_hex(tid);
  return pid;
}

// Drive SYS_PROC_REAP to completion, yielding through SYSERR_AGAIN
// (culling/drain may lag the death by a few dispatches).
static void reap_child(uint64_t pid) {
  uint64_t steps = 0;
  while (1) {
    uint64_t rc = sys_proc_reap(pid);
    if (rc == REAP_DONE) {
      break;
    }
    if (rc == SYSERR_AGAIN) {
      sys_yield();
      continue;
    }
    if (rc != REAP_MORE) {
      print("tests: REAP FAILED rc=");
      print_hex(rc);
      return;
    }
    steps++;
  }
  print("tests: reaped in bounded steps=");
  print_hex(steps);
}

// Wait for the next KEV_CHILD_DEAD on the tree channel and consume it.
static void await_child_death(struct kring *tch) {
  struct kcqe cqe;
  kring_wait_cqe(tch, &cqe);
  print(cqe.type == KEV_CHILD_DEAD ? "tests: KEV_CHILD_DEAD pid="
                                   : "tests: BAD TREE EVENT pid=");
  print_hex(cqe.a);
  kring_ack(tch); // consumption ack
}

// A second thread proves that an owned, unshared block is a local event:
// the durable sequence closes an early-wake race, while the doorbell wakes
// the owner-side waiter when the first thread is already parked.
static void local_event_waker(uint64_t event_base) {
  for (int i = 0; i < 64; i++) {
    sys_yield();
  }
  volatile uint32_t *seq = (volatile uint32_t *)event_base;
  __atomic_store_n(seq, 1, __ATOMIC_RELEASE);
  sys_block_doorbell(event_base);
  sys_exit();
}

static void test_local_event(void) {
  uint64_t event = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t stack = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  volatile uint32_t *seq = (volatile uint32_t *)event;
  uint64_t tid = sys_thread_spawn(sys_getpid(), (uint64_t)local_event_waker,
                                  stack + 4096, event);
  print("tests: local event waker tid=");
  print_hex(tid);
  uint64_t rc = sys_block_wait(seq, 0);
  print(rc == 0 && __atomic_load_n(seq, __ATOMIC_ACQUIRE) == 1
            ? "tests: local wait/doorbell ok\n"
            : "tests: LOCAL WAIT/DOORBELL FAILED\n");
  // The waker may still be exiting on its stack; both allocations are
  // reclaimed with the process rather than racing its TCB teardown here.
}

static void test_process_tree(struct kring *tch) {
  // --- Child 1: full channel life cycle ---------------------------------
  uint64_t boot_ch = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  volatile uint32_t *req = (volatile uint32_t *)(boot_ch + CH_REQ_OFF);
  volatile uint32_t *resp = (volatile uint32_t *)(boot_ch + CH_RESP_OFF);
  volatile char *msg = (volatile char *)(boot_ch + CH_MSG_OFF);
  static const char hello[] = "hello across the bootstrap channel";
  for (uint64_t i = 0; i <= sizeof(hello) - 1; i++) {
    msg[i] = hello[i];
  }

  uint64_t c1 = spawn_child(child_main, boot_ch);

  // Request/response over the pre-seeded channel (we own the block).
  __atomic_store_n(req, 1, __ATOMIC_RELEASE);
  sys_block_doorbell(boot_ch);
  sys_block_wait(resp, 0);
  while (__atomic_load_n(resp, __ATOMIC_ACQUIRE) == 0) {
    sys_yield();
  }
  print("tests: child served: ");
  uint64_t mlen = 0;
  while (mlen < CH_MSG_MAX && msg[mlen] != '\0') {
    mlen++;
  }
  sys_debug_write((const void *)msg, mlen);
  print("\n");

  // Give the child time to park on the block again (we want to exercise
  // the woken-from-park path, not just the fail-fast one) ...
  for (int i = 0; i < 64; i++) {
    sys_yield();
  }
  // ... then free the channel block: the child wakes SYSERR_DEAD and
  // exits; its death shows up on our tree channel; then we reap it.
  print("tests: vm_free(boot_ch) rc=");
  print_hex(sys_vm_free(boot_ch));
  await_child_death(tch);
  reap_child(c1);

  // A reaped pid is gone for good (never reused): all verbs refuse it.
  print("tests: reap of reaped pid rc=");
  print_hex(sys_proc_reap(c1));

  // --- Child 2: kill a running process ----------------------------------
  uint64_t c2 = spawn_child(child_spin_main, 0);
  sys_yield(); // let the victim actually run
  print("tests: proc_kill rc=");
  print_hex(sys_proc_kill(c2));
  await_child_death(tch);
  reap_child(c2);

  // Kill authority: only descendants (not self, not strangers).
  print("tests: kill self rc=");
  print_hex(sys_proc_kill(sys_getpid()));

  // --- Child 3: kill a CPU-bound process (preemption test) --------------
  // The burner never syscalls, so before timer preemption this could
  // never terminate: the victim would spin in ring 3 forever, its thread
  // never culled and its AS pinned on whatever CPU ran it.
  uint64_t c3 = spawn_child(child_burn_main, 0);
  for (int i = 0; i < 64; i++) {
    sys_yield(); // let the burner get dispatched somewhere
  }
  print("tests: proc_kill(burner) rc=");
  print_hex(sys_proc_kill(c3));
  await_child_death(tch);
  reap_child(c3);

}

void _start(uint64_t arg) {
  (void)arg;
  print("tests: hello from the ring-3 suite (a real process, not init)\n");
  // If the userland loader (lib/sys pe.c) relocated us wrong, this jumps
  // into the weeds right here.
  g_reloc_fn(g_reloc_str);

  print("tests: pid=");
  print_hex(sys_getpid());

  sys_yield();
  print("tests: back from yield\n");

  test_memory();
  test_local_event();

  // Tree channel: our children's deaths arrive here (we are a mid-tree
  // process now — init watches for OUR death the same way).
  struct kring tch;
  print("tests: kring_create(tch, -3) rc=");
  print_hex(kring_create(&tch, KSCHEME_TREE, 4096));
  test_process_tree(&tch);

  print("tests: all tests done\n");
  // Unlike the old hello.c-as-init, this process may exit: init reaps
  // us, which frees our image, stack, and the tree-channel block.
  sys_exit();
}
