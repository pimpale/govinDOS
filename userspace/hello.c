// Ring-3 test suite for govindos, compiled to PE with the same clang/
// lld-link toolchain as the kernel and loaded by the kernel's PE loader
// (kernel/src/pe.c). Exercises the whole current syscall surface:
// debug_write (incl. pointer validation), yield, getpid/getuid, vm_map/
// vm_unmap, the blocking dummy device, and the submission/completion
// ring.
//
// Freestanding: no libc, no imports (the loader rejects import tables).
// Built with -mgeneral-regs-only — the kernel does not save FPU/SSE state
// across context switches yet. Care is taken to avoid struct copies that
// would make clang emit memcpy calls.

#include <stdint.h>

#define SYS_DEBUG_WRITE 0
#define SYS_EXIT 1
#define SYS_YIELD 2
#define SYS_GETUID 3
#define SYS_GETPID 4
#define SYS_VM_MAP 5
#define SYS_VM_UNMAP 6
#define SYS_DUMMY_READ 7
#define SYS_RING_CREATE 8
#define SYS_RING_ENTER 9
#define SYS_RING_WAIT 10
#define SYS_VM_PROTECT 11
#define SYS_VM_SHARE 12
#define SYS_VM_UNSHARE 13
#define SYS_SESSION_LISTEN 14
#define SYS_SESSION_ACCEPT 15
#define SYS_SESSION_CONNECT 16
#define SYS_SESSION_DOORBELL 17
#define SYS_SESSION_WAIT 18

#define VM_PROT_READ 1
#define VM_PROT_WRITE 2

// Ring ABI (mirror of kernel/src/ring.h — offsets are fixed contract).
#define RING_SQ_HEAD_OFF 0
#define RING_SQ_TAIL_OFF 4
#define RING_CQ_HEAD_OFF 8
#define RING_CQ_TAIL_OFF 12
#define RING_SQ_OFF 64
#define RING_CQ_OFF 832
#define RING_MASK 15

// Bootstrap arg (passed by the kernel in rcx to _start). 0 = legacy single-
// process suite; this magic = session server; anything else = session client
// connecting to the server whose pid is the arg value.
#define SESSION_SERVER_MAGIC 0x5E510001ull

#define RING_OP_NOP 0
#define RING_OP_DEBUG_WRITE 1
#define RING_OP_DUMMY_READ 2

struct ring_sqe {
  uint32_t opcode;
  uint32_t flags;
  uint64_t user_data;
  uint64_t args[4];
};

struct ring_cqe {
  uint64_t user_data;
  int64_t result;
};

// int 0x80 ABI: rax = nr, args in rcx/rdx/r8/r9, result in rax; the
// kernel preserves all other registers.
static inline uint64_t sys3(uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2) {
  uint64_t ret;
  register uint64_t rc __asm__("rcx") = a0;
  register uint64_t rd __asm__("rdx") = a1;
  register uint64_t r8 __asm__("r8") = a2;
  __asm__ volatile("int $0x80"
                   : "=a"(ret), "+r"(rc), "+r"(rd), "+r"(r8)
                   : "a"(nr)
                   : "memory");
  return ret;
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

  // Share error paths (there is no second process to rendezvous with, so
  // only the failure modes are reachable from here; the success path is
  // covered by the kernel's boot-time selftest).
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

static void test_ring(void) {
  print("pe: ring test (nop, dummy_read, debug_write)\n");
  uint64_t rb = sys0(SYS_RING_CREATE);
  print("pe: ring at ");
  print_hex(rb);

  volatile struct ring_sqe *sq = (volatile struct ring_sqe *)(rb + RING_SQ_OFF);
  volatile struct ring_cqe *cq = (volatile struct ring_cqe *)(rb + RING_CQ_OFF);
  uint32_t *sq_tail = (uint32_t *)(rb + RING_SQ_TAIL_OFF);
  uint32_t *cq_head = (uint32_t *)(rb + RING_CQ_HEAD_OFF);
  uint32_t *cq_tail = (uint32_t *)(rb + RING_CQ_TAIL_OFF);

  // sq[0]: NOP. sq[1]: DUMMY_READ (blocks the worker kthread, not us).
  // sq[2]: DEBUG_WRITE executed by the worker.
  static const char rmsg[] = "pe(ring): debug_write via the ring worker\n";
  sq[0].opcode = RING_OP_NOP;
  sq[0].user_data = 0x1111;
  sq[1].opcode = RING_OP_DUMMY_READ;
  sq[1].user_data = 0x2222;
  sq[2].opcode = RING_OP_DEBUG_WRITE;
  sq[2].user_data = 0x3333;
  sq[2].args[0] = (uint64_t)rmsg;
  sq[2].args[1] = sizeof(rmsg) - 1;

  // Publish, then ring the doorbell.
  __atomic_store_n(sq_tail, 3, __ATOMIC_RELEASE);
  sys0(SYS_RING_ENTER);

  // Drain all three completions; sleep in ring_wait when caught up.
  uint32_t head = 0;
  while (head < 3) {
    sys1(SYS_RING_WAIT, head);
    while (head != __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE)) {
      print("pe(ring): user_data=");
      print_hex(cq[head & RING_MASK].user_data);
      print("pe(ring): result=");
      print_hex((uint64_t)cq[head & RING_MASK].result);
      head++;
      __atomic_store_n(cq_head, head, __ATOMIC_RELEASE);
    }
  }
}

void _start(void) {
  print("pe: hello from a C program loaded as PE!\n");
  g_reloc_fn(g_reloc_str);

  print("pe: pid=");
  print_hex(sys0(SYS_GETPID));
  print("pe: uid=");
  print_hex(sys0(SYS_GETUID));

  sys0(SYS_YIELD);
  print("pe: back from yield\n");

  test_memory();

  print("pe: blocking on dummy device...\n");
  print_hex(sys0(SYS_DUMMY_READ));

  test_ring();

  print("pe: all tests done, exiting\n");
  sys0(SYS_EXIT);
  __builtin_unreachable();
}
