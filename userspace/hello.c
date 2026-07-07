// init + ring-3 test suite for govindos, compiled to PE with the same
// clang/lld-link toolchain as the kernel and loaded by the kernel's
// boot-only loader (kernel/src/pe.c) as the root of the process tree.
//
// Everything below init is built here, parent-driven, the way real
// userspace will do it (ipc-process-design.md §5): PROC_CREATE an
// embryo, VM_MOVE it a stack, VM_SHARE it this very image read-execute
// (SASOS: the child runs the same code at the same addresses), pre-seed
// a bootstrap channel, THREAD_SPAWN its first thread. Children exercise
// the channel data plane from the far side; kill/reap/tree-events are
// exercised from this side.
//
// Freestanding: no libc, no imports (the loader rejects import tables).
// Built with -mgeneral-regs-only — the kernel does not save FPU/SSE
// state across context switches yet. Child code must not write globals:
// its view of the image is read-execute (per-view W^X), so it works in
// stack locals only.

#include <stdint.h>

#define SYS_DEBUG_WRITE 0
#define SYS_EXIT 1
#define SYS_YIELD 2
#define SYS_GETUID 3
#define SYS_GETPID 4
#define SYS_VM_MAP 5
#define SYS_VM_UNMAP 6
#define SYS_VM_PROTECT 7
#define SYS_VM_SHARE 8
#define SYS_VM_UNSHARE 9
#define SYS_VM_MOVE 10
#define SYS_BLOCK_DOORBELL 11
#define SYS_BLOCK_WAIT 12
#define SYS_PROC_CREATE 13
#define SYS_THREAD_SPAWN 14
#define SYS_PROC_KILL 15
#define SYS_PROC_REAP 16

#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXEC 4

#define REAP_DONE 0
#define REAP_MORE 1
#define SYSERR_DEAD ((uint64_t)-7)
#define SYSERR_AGAIN ((uint64_t)-8)

// Kernel ring ABI (mirror of kernel/src/channel.h). Header at 0, then
// sq[nslots], then cq[nslots]; 32-byte entries; nslots = 32 << order.
#define KRING_HDR_SIZE 64
#define KRING_CQ_COUNT_OFF 0
#define KRING_CQ_HEAD_OFF 4
#define KRING_SQ_TAIL_OFF 12
#define KEV_SHARE_LO 1 // low bits; the type is (1<<63)|n
#define KEV_CHILD_DEAD_LO 2
#define KEV_READY_LO 3
#define KEV_DEAD_LO 4
#define KSCHEME_SHARES ((uint64_t)-1)
#define KSCHEME_GROUPS ((uint64_t)-2)
#define KSCHEME_TREE ((uint64_t)-3)
#define KGROUP_ADD 1
#define KGROUP_DEL 2

struct ksqe {
  uint64_t op;
  uint64_t a, b, c;
};

struct kcqe {
  uint64_t type;
  uint64_t a, b;
  uint64_t status;
};

// SYSCALL ABI: rax = nr, args in r10/rdx/r8/r9, result in rax. rcx and
// r11 are clobbered by the instruction itself (return rip/rflags); the
// kernel preserves every other register.
static inline uint64_t sys4(uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3) {
  uint64_t ret;
  register uint64_t r10 __asm__("r10") = a0;
  register uint64_t rdx __asm__("rdx") = a1;
  register uint64_t r8 __asm__("r8") = a2;
  register uint64_t r9 __asm__("r9") = a3;
  __asm__ volatile("syscall"
                   : "=a"(ret), "+r"(r10), "+r"(rdx), "+r"(r8), "+r"(r9)
                   : "a"(nr)
                   : "rcx", "r11", "memory");
  return ret;
}

static inline uint64_t sys3(uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2) {
  return sys4(nr, a0, a1, a2, 0);
}
static inline uint64_t sys2(uint64_t nr, uint64_t a0, uint64_t a1) {
  return sys3(nr, a0, a1, 0);
}
static inline uint64_t sys1(uint64_t nr, uint64_t a0) { return sys2(nr, a0, 0); }
static inline uint64_t sys0(uint64_t nr) { return sys2(nr, 0, 0); }

static uint64_t str_len(const char *s) {
  uint64_t len = 0;
  while (s[len] != '\0') {
    len++;
  }
  return len;
}

static void print(const char *s) { sys2(SYS_DEBUG_WRITE, (uint64_t)s, str_len(s)); }

static void print_hex(uint64_t v) {
  char buf[17];
  for (int i = 15; i >= 0; i--) {
    uint64_t d = v & 0xF;
    buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  buf[16] = '\n';
  sys2(SYS_DEBUG_WRITE, (uint64_t)buf, 17);
}

// Absolute addresses baked into .data at link time: these force DIR64
// base relocations that the compiler cannot fold into rip-relative
// accesses (volatile). If the loader rebases wrong, this jumps into the
// weeds instead of printing.
typedef void (*printer_t)(const char *);
static volatile printer_t g_reloc_fn = print;
static const char g_reloc_probe[] = "pe: relocated fn ptr + string work\n";
static const char *volatile g_reloc_str = g_reloc_probe;

// The image's load address == the base of the ublock the loader put it
// in (pe.c allocates the block and copies to its base), so this is what
// VM_SHARE wants. lld-link provides the pseudo-symbol.
extern char __ImageBase;

static void test_memory(void) {
  // Map two pages RW, print through them, unmap.
  uint64_t base = sys2(SYS_VM_MAP, 8192, VM_PROT_READ | VM_PROT_WRITE);
  print("pe: vm_map base=");
  print_hex(base);

  const char *msg = "pe: printing from vm_map'd page\n";
  const char *msg2 = "pe: printing from a read-only page\n";
  char *pg = (char *)base;
  char *pg2 = (char *)(base + 4096);
  uint64_t len = str_len(msg);
  uint64_t len2 = str_len(msg2);
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
  print("child: hello (spawned by init)\n");

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
    if ((cq[i].type & 0xFF) == KEV_SHARE_LO &&
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

// Build a child process the parent-driven way and return its pid.
// entry runs on our shared image; boot_ch (may be 0) is its argument.
static uint64_t spawn_child(void (*entry)(uint64_t), uint64_t boot_ch) {
  uint64_t pid = sys0(SYS_PROC_CREATE);
  print("init: proc_create pid=");
  print_hex(pid);

  // Stack: built here, ownership transferred into the embryo.
  uint64_t stack = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("init: vm_move(stack) rc=");
  print_hex(sys2(SYS_VM_MOVE, stack, pid));
  // The mover keeps no view: reading through it must now fail.
  print("init: debug_write through moved stack rc=");
  print_hex(sys2(SYS_DEBUG_WRITE, stack, 8));

  // Code: share our own image read-execute. SASOS means the child sees
  // it at the same address, so `entry` is valid over there too.
  print("init: vm_share(image, RX) rc=");
  print_hex(sys3(SYS_VM_SHARE, (uint64_t)&__ImageBase, pid,
                 VM_PROT_READ | VM_PROT_EXEC));

  if (boot_ch != 0) {
    // Pre-seed the bootstrap channel while the child is an embryo — the
    // seL4/Xen answer to how strangers ever get introduced.
    print("init: vm_share(boot_ch) rc=");
    print_hex(sys3(SYS_VM_SHARE, boot_ch, pid, VM_PROT_READ | VM_PROT_WRITE));
  }

  uint64_t tid =
      sys4(SYS_THREAD_SPAWN, pid, (uint64_t)entry, stack + 4096, boot_ch);
  print("init: thread_spawn tid=");
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
      print("init: REAP FAILED rc=");
      print_hex(rc);
      return;
    }
    steps++;
  }
  print("init: reaped in bounded steps=");
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
  uint64_t ok = (cq[*seen & 31].type & 0xFF) == KEV_CHILD_DEAD_LO;
  print(ok ? "init: KEV_CHILD_DEAD pid=" : "init: BAD TREE EVENT pid=");
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
// the way. Consumption is acked with a doorbell so level state replays.
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
    while (*seen != avail) {
      uint64_t t = cq[*seen & 31].type;
      uint64_t a = cq[*seen & 31].a;
      (*seen)++;
      if (t >> 63) {
        print("init(group): event lo=");
        print_hex(((t & 0xFF) << 32) | (a & 0xFFFFFFFF));
        if ((t & 0xFF) == type_lo && (cookie == 0 || a == cookie)) {
          hit = 1;
        }
      } else {
        print("init(group): completion op=");
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
  print("init: vm_share(g, -2) rc=");
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
  uint64_t c3 = spawn_child(child_main, boot_ch);
  group_submit(g, &g_sq, KGROUP_ADD, boot_ch, 0xC0FFEE);

  __atomic_store_n(req, 1, __ATOMIC_RELEASE);
  sys1(SYS_BLOCK_DOORBELL, boot_ch);

  // The child's reply doorbell lands as KEV_READY{C0FFEE} in the group.
  group_await(g, &g_seen, KEV_READY_LO, 0xC0FFEE);
  print(__atomic_load_n(resp, __ATOMIC_ACQUIRE) == 1
            ? "init: group heard the reply doorbell ok\n"
            : "init: GROUP WOKE WITHOUT RESPONSE\n");

  // Let the child park again, then revoke the channel: the registration
  // must turn into KEV_DEAD{C0FFEE} (POLLHUP, auto-removed)...
  for (int i = 0; i < 64; i++) {
    sys0(SYS_YIELD);
  }
  print("init: vm_unmap(boot_ch) rc=");
  print_hex(sys2(SYS_VM_UNMAP, boot_ch, 4096));
  group_await(g, &g_seen, KEV_DEAD_LO, 0xC0FFEE);

  // ...and the child's death reaches us through the registered tree
  // channel: KEV_READY{7EE} in the group, KEV_CHILD_DEAD underneath.
  group_await(g, &g_seen, KEV_READY_LO, 0x7EE);
  await_child_death(tch, tree_seen);
  reap_child(c3);

  // DEL the tree registration and drop the group; the tree channel is
  // directly parkable again (which _start relies on).
  group_submit(g, &g_sq, KGROUP_DEL, tch, 0);
  print("init: vm_unmap(group) rc=");
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
  print("init: child served: ");
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
  print("init: vm_unmap(boot_ch) rc=");
  print_hex(sys2(SYS_VM_UNMAP, boot_ch, 4096));
  await_child_death(tch, &tree_seen);
  reap_child(c1);

  // A reaped pid is gone for good (never reused): all verbs refuse it.
  print("init: reap of reaped pid rc=");
  print_hex(sys1(SYS_PROC_REAP, c1));

  // --- Child 2: kill a running process ----------------------------------
  uint64_t c2 = spawn_child(child_spin_main, 0);
  sys0(SYS_YIELD); // let the victim actually run
  print("init: proc_kill rc=");
  print_hex(sys1(SYS_PROC_KILL, c2));
  await_child_death(tch, &tree_seen);
  reap_child(c2);

  // Kill authority: only descendants (not self, not strangers).
  print("init: kill self rc=");
  print_hex(sys1(SYS_PROC_KILL, sys0(SYS_GETPID)));

  // --- Child 3: multiplex through a wait-group --------------------------
  test_wait_group(tch, &tree_seen);

  *tree_seen_out = tree_seen;
}

void _start(uint64_t arg) {
  (void)arg;
  print("init: hello from the root of the process tree!\n");
  g_reloc_fn(g_reloc_str);

  print("init: pid=");
  print_hex(sys0(SYS_GETPID));
  print("init: uid=");
  print_hex(sys0(SYS_GETUID));

  sys0(SYS_YIELD);
  print("init: back from yield\n");

  test_memory();

  // Tree channel: children's deaths arrive here, and it doubles as
  // init's forever-park spot at the end (a kernel channel is always
  // waitable — no peer needs to stay alive).
  uint64_t tch = sys2(SYS_VM_MAP, 4096, VM_PROT_READ | VM_PROT_WRITE);
  print("init: vm_share(tch, -3) rc=");
  print_hex(sys3(SYS_VM_SHARE, tch, KSCHEME_TREE, 0));
  uint32_t tree_seen = 0;
  test_process_tree(tch, &tree_seen);

  print("init: all tests done\n");
  // init must never exit (that's a kernel panic). Nothing left to do:
  // park on the tree channel; no child remains, so nothing ever posts.
  volatile uint32_t *tree_count = (volatile uint32_t *)(tch + KRING_CQ_COUNT_OFF);
  while (1) {
    sys2(SYS_BLOCK_WAIT, (uint64_t)tree_count, tree_seen);
    sys0(SYS_YIELD);
  }
}
