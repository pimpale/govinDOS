// The ring-3 test suite for govindos, shipped in the initfs and spawned
// by init through the userland PE loader (lib/upe.c) as a real separate
// process — which makes every boot regression-test that loader too.
// Descended from the original hello.c, which doubled as init and the
// suite until the boot-init design split the roles
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
// Freestanding: no libc, no imports (the loader rejects import tables).
// Built with -mgeneral-regs-only — the kernel preserves x87/SSE across
// switches (fxsave at park, fxrstor at resume) but not AVX, so YMM+
// state must stay unused. Child code must not write globals: its view
// of the image is read-execute (per-view W^X), so it works in stack
// locals only.

#include <stdint.h>

#include "usys.h"

// Absolute addresses baked into .data at link time: these force DIR64
// base relocations that the compiler cannot fold into rip-relative
// accesses (volatile). If the loader rebases wrong, this jumps into the
// weeds instead of printing.
typedef void (*printer_t)(const char *);
static volatile printer_t g_reloc_fn = print;
static const char g_reloc_probe[] = "pe: relocated fn ptr + string work\n";
static const char *volatile g_reloc_str = g_reloc_probe;

static void test_memory(void) {
  // Map two pages RW, print through them, unmap.
  uint64_t base = sys2(SYS_VM_MAP, 8192, VM_PROT_READ | VM_PROT_WRITE);
  print("pe: vm_map base=");
  print_hex(base);

  const char *msg = "pe: printing from vm_map'd page\n";
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
  sys2(SYS_DEBUG_WRITE, base, len);

  // vm_protect: drop the second page to read-only in our own view.
  // Reads (debug_write) still work through it.
  print("pe: vm_protect(page2, RO) rc=");
  print_hex(sys3(SYS_VM_PROTECT, base + 4096, 4096, VM_PROT_READ));
  sys2(SYS_DEBUG_WRITE, base + 4096, len2);

  // Guard view (prot=0): the kernel must now refuse even reads there.
  print("pe: vm_protect(page2, none) rc=");
  print_hex(sys3(SYS_VM_PROTECT, base + 4096, 4096, 0));
  print("pe: debug_write through guarded page rc=");
  print_hex(sys2(SYS_DEBUG_WRITE, base + 4096, 16));

  // Restore and unmap. Partial unmap must be rejected (blocks are the
  // unit); exact unmap succeeds.
  print("pe: vm_protect(page2, RW) rc=");
  print_hex(sys3(SYS_VM_PROTECT, base + 4096, 4096,
                 VM_PROT_READ | VM_PROT_WRITE));
  print("pe: partial vm_unmap rc=");
  print_hex(sys2(SYS_VM_UNMAP, base, 4096));
  print("pe: vm_unmap rc=");
  print_hex(sys2(SYS_VM_UNMAP, base, 8192));

  // Double free must fail.
  print("pe: double vm_unmap rc=");
  print_hex(sys2(SYS_VM_UNMAP, base, 8192));

  // Share error paths.
  uint64_t blk = sys2(SYS_VM_MAP, 4096, VM_PROT_READ);
  print("pe: vm_share to bogus pid rc=");
  print_hex(sys3(SYS_VM_SHARE, blk, 0xdead, VM_PROT_READ));
  print("pe: vm_share to self rc=");
  print_hex(sys3(SYS_VM_SHARE, blk, sys0(SYS_GETPID), VM_PROT_READ));
  print("pe: vm_unshare of owned block rc=");
  print_hex(sys1(SYS_VM_UNSHARE, blk));
  print("pe: cleanup vm_unmap rc=");
  print_hex(sys2(SYS_VM_UNMAP, blk, 4096));

  // The kernel must refuse to touch non-PAGE_U memory on our behalf
  // (expect SYSERR_FAULT, ...fffe).
  print("pe: kernel-ptr debug_write rc=");
  print_hex(sys2(SYS_DEBUG_WRITE, 0x1000, 16));
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
  // we were still an embryo, so its KEV_SHARE must replay right here.
  uint64_t ch = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("child: vm_share(ch, -1) rc=");
  print_hex(sys3(SYS_VM_SHARE, ch, KSCHEME_SHARES, 0));
  volatile uint32_t *cq_count = (volatile uint32_t *)(ch + KRING_CQ_COUNT_OFF);
  volatile struct kcqe *cq =
      (volatile struct kcqe *)(ch + KRING_HDR_SIZE + 32 * 32);
  sys2(SYS_BLOCK_WAIT, (uint64_t)cq_count, 0);
  while (__atomic_load_n(cq_count, __ATOMIC_ACQUIRE) == 0) {
    sys0(SYS_YIELD);
  }
  uint64_t seen = __atomic_load_n(cq_count, __ATOMIC_ACQUIRE);
  uint64_t found = 0;
  for (uint64_t i = 0; i < seen; i++) {
    if ((cq[i].type & 0xFF) == KEV_LO(KEV_SHARE) &&
        (cq[i].b & ~0xFFFull) == boot_ch) {
      found = 1;
    }
  }
  print(found ? "child: bootstrap share replayed ok\n"
              : "child: BOOTSTRAP SHARE MISSING\n");

  // Serve one request on the bootstrap block (we are the sharer side).
  volatile uint32_t *req = (volatile uint32_t *)(boot_ch + CH_REQ_OFF);
  volatile uint32_t *resp = (volatile uint32_t *)(boot_ch + CH_RESP_OFF);
  volatile char *msg = (volatile char *)(boot_ch + CH_MSG_OFF);
  sys2(SYS_BLOCK_WAIT, (uint64_t)req, 0);
  while (__atomic_load_n(req, __ATOMIC_ACQUIRE) == 0) {
    sys0(SYS_YIELD);
  }
  for (uint64_t i = 0; i < CH_MSG_MAX && msg[i] != '\0'; i++) {
    if (msg[i] >= 'a' && msg[i] <= 'z') {
      msg[i] = (char)(msg[i] - 'a' + 'A');
    }
  }
  __atomic_store_n(resp, 1, __ATOMIC_RELEASE);
  sys1(SYS_BLOCK_DOORBELL, boot_ch);

  // Park until init frees the bootstrap block: the revoke path must wake
  // us with SYSERR_DEAD. Don't touch the block afterwards.
  uint64_t rc = sys2(SYS_BLOCK_WAIT, (uint64_t)req, 1);
  print(rc == SYSERR_DEAD ? "child: revoke wake ok, exiting\n"
                          : "child: REVOKE WAKE WRONG\n");
  sys0(SYS_EXIT);
}

// First thread of the kill-test child: spins in yield until killed.
static void child_spin_main(uint64_t arg) {
  (void)arg;
  print("victim: spinning until killed\n");
  while (1) {
    sys0(SYS_YIELD);
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
  uint64_t pid = sys0(SYS_PROC_CREATE);
  print("tests: proc_create pid=");
  print_hex(pid);

  // Stack: built here, ownership transferred into the embryo.
  uint64_t stack = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("tests: vm_move(stack) rc=");
  print_hex(sys2(SYS_VM_MOVE, stack, pid));
  // The mover keeps no view: reading through it must now fail.
  print("tests: debug_write through moved stack rc=");
  print_hex(sys2(SYS_DEBUG_WRITE, stack, 8));

  // Code: share our own image read-execute. SASOS means the child sees
  // it at the same address, so `entry` is valid over there too.
  print("tests: vm_share(image, RX) rc=");
  print_hex(sys3(SYS_VM_SHARE, (uint64_t)&__ImageBase, pid,
                 VM_PROT_READ | VM_PROT_EXEC));

  if (boot_ch != 0) {
    // Pre-seed the bootstrap channel while the child is an embryo — the
    // seL4/Xen answer to how strangers ever get introduced.
    print("tests: vm_share(boot_ch) rc=");
    print_hex(sys3(SYS_VM_SHARE, boot_ch, pid, VM_PROT_READ | VM_PROT_WRITE));
  }

  uint64_t tid =
      sys4(SYS_THREAD_SPAWN, pid, (uint64_t)entry, stack + 4096, boot_ch);
  print("tests: thread_spawn tid=");
  print_hex(tid);
  return pid;
}

// Drive SYS_PROC_REAP to completion, yielding through SYSERR_AGAIN
// (culling/drain may lag the death by a few dispatches).
static void reap_child(uint64_t pid) {
  uint64_t steps = 0;
  while (1) {
    uint64_t rc = sys1(SYS_PROC_REAP, pid);
    if (rc == REAP_DONE) {
      break;
    }
    if (rc == SYSERR_AGAIN) {
      sys0(SYS_YIELD);
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
  print(ok ? "tests: KEV_CHILD_DEAD pid=" : "tests: BAD TREE EVENT pid=");
  print_hex(cq[*seen & 31].a);
  (*seen)++;
  __atomic_store_n(cq_head, *seen, __ATOMIC_RELEASE);
  sys1(SYS_BLOCK_DOORBELL, tch); // consumption ack
}

// ---------------------------------------------------------------------------
// Wait-group (scheme -2) helpers: SQE submission + CQ draining
// ---------------------------------------------------------------------------

static void group_submit(uint64_t g, uint32_t *sq_tail_shadow, uint64_t op,
                         uint64_t a, uint64_t b) {
  volatile struct ksqe *sq = (volatile struct ksqe *)(g + KRING_HDR_SIZE);
  volatile uint32_t *sq_tail = (volatile uint32_t *)(g + KRING_SQ_TAIL_OFF);
  uint32_t t = *sq_tail_shadow;
  sq[t & 31].op = op;
  sq[t & 31].a = a;
  sq[t & 31].b = b;
  sq[t & 31].c = 0;
  *sq_tail_shadow = t + 1;
  __atomic_store_n(sq_tail, t + 1, __ATOMIC_RELEASE);
  sys1(SYS_BLOCK_DOORBELL, g);
}

// Wait until an event with this low type (and, unless 0, this cookie in
// `a`) shows up in the group's CQ, consuming and printing everything on
// the way — but nothing past the hit: two awaited events can land in
// one batch (scheduling-dependent), and an event consumed while hunting
// a different one would be lost to the next await. Consumption is acked
// with a doorbell so level state replays.
static void group_await(uint64_t g, uint32_t *seen, uint64_t type_lo,
                        uint64_t cookie) {
  volatile uint32_t *cq_count = (volatile uint32_t *)(g + KRING_CQ_COUNT_OFF);
  volatile uint32_t *cq_head = (volatile uint32_t *)(g + KRING_CQ_HEAD_OFF);
  volatile struct kcqe *cq =
      (volatile struct kcqe *)(g + KRING_HDR_SIZE + 32 * 32);
  while (1) {
    sys2(SYS_BLOCK_WAIT, (uint64_t)cq_count, *seen);
    while (__atomic_load_n(cq_count, __ATOMIC_ACQUIRE) == *seen) {
      sys0(SYS_YIELD);
    }
    uint32_t avail = __atomic_load_n(cq_count, __ATOMIC_ACQUIRE);
    uint64_t hit = 0;
    while (*seen != avail && !hit) {
      uint64_t t = cq[*seen & 31].type;
      uint64_t a = cq[*seen & 31].a;
      (*seen)++;
      if (t >> 63) {
        print("tests(group): event lo=");
        print_hex(((t & 0xFF) << 32) | (a & 0xFFFFFFFF));
        if ((t & 0xFF) == type_lo && (cookie == 0 || a == cookie)) {
          hit = 1;
        }
      } else {
        print("tests(group): completion op=");
        print_hex((t << 32) | (cq[(*seen - 1) & 31].status & 0xFFFFFFFF));
      }
    }
    __atomic_store_n(cq_head, *seen, __ATOMIC_RELEASE);
    sys1(SYS_BLOCK_DOORBELL, g); // consumption ack
    if (hit) {
      return;
    }
  }
}

// Multiplexing server: one park spot (the group) hears a user channel, a
// registered kernel channel (the tree channel), and revocation.
static void test_wait_group(uint64_t tch, uint32_t *tree_seen) {
  uint64_t g = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("tests: vm_share(g, -2) rc=");
  print_hex(sys3(SYS_VM_SHARE, g, KSCHEME_GROUPS, 0));
  uint32_t g_seen = 0;
  uint32_t g_sq = 0;

  // Register the tree channel (a kernel channel we own): child deaths
  // will now show up as KEV_READY{0x7EE} here.
  group_submit(g, &g_sq, KGROUP_ADD, tch, 0x7EE);
  // Group-in-group must be refused (completion carries the error).
  group_submit(g, &g_sq, KGROUP_ADD, g, 0xBAD);

  // Bootstrap channel + child, as before — but init multiplexes on the
  // group instead of parking on the block.
  uint64_t boot_ch = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  volatile uint32_t *req = (volatile uint32_t *)(boot_ch + CH_REQ_OFF);
  volatile uint32_t *resp = (volatile uint32_t *)(boot_ch + CH_RESP_OFF);
  volatile char *msg = (volatile char *)(boot_ch + CH_MSG_OFF);
  static const char hello2[] = "hello through the wait-group";
  for (uint64_t i = 0; i <= sizeof(hello2) - 1; i++) {
    msg[i] = hello2[i];
  }
  uint64_t c4 = spawn_child(child_main, boot_ch);
  group_submit(g, &g_sq, KGROUP_ADD, boot_ch, 0xC0FFEE);

  __atomic_store_n(req, 1, __ATOMIC_RELEASE);
  sys1(SYS_BLOCK_DOORBELL, boot_ch);

  // The child's reply doorbell lands as KEV_READY{C0FFEE} in the group.
  group_await(g, &g_seen, KEV_LO(KEV_READY), 0xC0FFEE);
  print(__atomic_load_n(resp, __ATOMIC_ACQUIRE) == 1
            ? "tests: group heard the reply doorbell ok\n"
            : "tests: GROUP WOKE WITHOUT RESPONSE\n");

  // Let the child park again, then revoke the channel: the registration
  // must turn into KEV_DEAD{C0FFEE} (POLLHUP, auto-removed)...
  for (int i = 0; i < 64; i++) {
    sys0(SYS_YIELD);
  }
  print("tests: vm_unmap(boot_ch) rc=");
  print_hex(sys2(SYS_VM_UNMAP, boot_ch, 4096));
  group_await(g, &g_seen, KEV_LO(KEV_DEAD), 0xC0FFEE);

  // ...and the child's death reaches us through the registered tree
  // channel: KEV_READY{7EE} in the group, KEV_CHILD_DEAD underneath.
  group_await(g, &g_seen, KEV_LO(KEV_READY), 0x7EE);
  await_child_death(tch, tree_seen);
  reap_child(c4);

  // DEL the tree registration and drop the group; the tree channel is
  // directly parkable again (which _start relies on).
  group_submit(g, &g_sq, KGROUP_DEL, tch, 0);
  print("tests: vm_unmap(group) rc=");
  print_hex(sys2(SYS_VM_UNMAP, g, 4096));
}

static void test_process_tree(uint64_t tch, uint32_t *tree_seen_out) {
  uint32_t tree_seen = 0;

  // --- Child 1: full channel life cycle ---------------------------------
  uint64_t boot_ch = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
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
  sys1(SYS_BLOCK_DOORBELL, boot_ch);
  sys2(SYS_BLOCK_WAIT, (uint64_t)resp, 0);
  while (__atomic_load_n(resp, __ATOMIC_ACQUIRE) == 0) {
    sys0(SYS_YIELD);
  }
  print("tests: child served: ");
  uint64_t mlen = 0;
  while (mlen < CH_MSG_MAX && msg[mlen] != '\0') {
    mlen++;
  }
  sys2(SYS_DEBUG_WRITE, (uint64_t)msg, mlen);
  print("\n");

  // Give the child time to park on the block again (we want to exercise
  // the woken-from-park path, not just the fail-fast one) ...
  for (int i = 0; i < 64; i++) {
    sys0(SYS_YIELD);
  }
  // ... then free the channel block: the child wakes SYSERR_DEAD and
  // exits; its death shows up on our tree channel; then we reap it.
  print("tests: vm_unmap(boot_ch) rc=");
  print_hex(sys2(SYS_VM_UNMAP, boot_ch, 4096));
  await_child_death(tch, &tree_seen);
  reap_child(c1);

  // A reaped pid is gone for good (never reused): all verbs refuse it.
  print("tests: reap of reaped pid rc=");
  print_hex(sys1(SYS_PROC_REAP, c1));

  // --- Child 2: kill a running process ----------------------------------
  uint64_t c2 = spawn_child(child_spin_main, 0);
  sys0(SYS_YIELD); // let the victim actually run
  print("tests: proc_kill rc=");
  print_hex(sys1(SYS_PROC_KILL, c2));
  await_child_death(tch, &tree_seen);
  reap_child(c2);

  // Kill authority: only descendants (not self, not strangers).
  print("tests: kill self rc=");
  print_hex(sys1(SYS_PROC_KILL, sys0(SYS_GETPID)));

  // --- Child 3: kill a CPU-bound process (preemption test) --------------
  // The burner never syscalls, so before timer preemption this could
  // never terminate: the victim would spin in ring 3 forever, its thread
  // never culled and its AS pinned on whatever CPU ran it.
  uint64_t c3 = spawn_child(child_burn_main, 0);
  for (int i = 0; i < 64; i++) {
    sys0(SYS_YIELD); // let the burner get dispatched somewhere
  }
  print("tests: proc_kill(burner) rc=");
  print_hex(sys1(SYS_PROC_KILL, c3));
  await_child_death(tch, &tree_seen);
  reap_child(c3);

  // --- Child 4: multiplex through a wait-group --------------------------
  test_wait_group(tch, &tree_seen);

  *tree_seen_out = tree_seen;
}

void _start(uint64_t arg) {
  (void)arg;
  print("tests: hello from the ring-3 suite (a real process, not init)\n");
  // If the userland loader (lib/upe.c) relocated us wrong, this jumps
  // into the weeds right here.
  g_reloc_fn(g_reloc_str);

  print("tests: pid=");
  print_hex(sys0(SYS_GETPID));
  print("tests: uid=");
  print_hex(sys0(SYS_GETUID));

  sys0(SYS_YIELD);
  print("tests: back from yield\n");

  test_memory();

  // Tree channel: our children's deaths arrive here (we are a mid-tree
  // process now — init watches for OUR death the same way).
  uint64_t tch = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("tests: vm_share(tch, -3) rc=");
  print_hex(sys3(SYS_VM_SHARE, tch, KSCHEME_TREE, 0));
  uint32_t tree_seen = 0;
  test_process_tree(tch, &tree_seen);

  print("tests: all tests done\n");
  // Unlike the old hello.c-as-init, this process may exit: init reaps
  // us, which frees our image, stack, and the tree-channel block.
  sys0(SYS_EXIT);
}
