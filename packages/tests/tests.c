// The ring-3 test suite for govindos, shipped in the initfs and spawned
// by init through the userland PE loader (gdoslib-dev/pe.c) as a real
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
// Kernel channels go through gdoslib-dev <kring.h> (so every boot exercises
// that library too); the bootstrap block stays hand-rolled — it's a
// *user* channel, its layout is our own convention, not kernel ABI.
//
// Freestanding: no libc, no imports (the loader rejects import tables).
// Built with -mgeneral-regs-only so ordinary compiler output does not hide
// which vector registers the xstate test below dirties explicitly. The kernel
// nevertheless enables and eagerly preserves its selected XSAVE state.
// Child code must not write globals: its view
// of the image is read-execute (per-view W^X), so it works in stack
// locals only.

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include <gdosabi/kring_shares.h>
#include <gdosabi/kring_tree.h>

#include <kring.h>
#include <pe.h>
#include <sys.h>

static _Thread_local uint64_t g_tls_initialized = 0x544c53494e495431ull;
static _Thread_local uint64_t g_tls_zero[4];

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
  print("pe: vm_size=");
  print_hex(sys_vm_size(base));
  print("pe: mid-block vm_size rc=");
  print_hex(sys_vm_size(base + 4096));

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

static void test_realloc(void) {
  uint8_t *ptr = malloc(17);
  if (ptr == nullptr || sys_vm_size((uint64_t)ptr) != 4096) {
    print("tests: REALLOC ALLOCATION FAILED\n");
    free(ptr);
    return;
  }

  for (uint8_t i = 0; i < 17; i++) {
    ptr[i] = (uint8_t)(i + 1);
  }

  uint8_t *grown = realloc(ptr, 4097);
  if (grown == nullptr) {
    print("tests: REALLOC GROW FAILED\n");
    free(ptr);
    return;
  }

  bool contents_ok = true;
  for (uint8_t i = 0; i < 17; i++) {
    if (grown[i] != (uint8_t)(i + 1)) {
      contents_ok = false;
    }
  }
  bool size_ok = sys_vm_size((uint64_t)grown) == 8192;
  uint8_t *shrunk = realloc(grown, 16);
  bool shrink_ok = shrunk == grown;
  uint64_t base = (uint64_t)shrunk;
  free(shrunk);
  bool free_ok = sys_vm_size(base) == SYSERR_PERM;

  print(contents_ok && size_ok && shrink_ok && free_ok
            ? "tests: realloc/vm_size ok\n"
            : "tests: REALLOC/VM_SIZE FAILED\n");
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
  struct kcqe cqe = {0};
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
  sys_proc_exit(0);
}

// First thread of the kill-test child: spins in yield until killed.
static void child_spin_main(uint64_t arg) {
  (void)arg;
  print("victim: spinning until killed\n");
  while (1) {
    sys_yield();
  }
}

// Announce that we reached the channel, then remain parked. Killing this
// process exercises lazy dead-waiter detachment and reap-owned TCB freeing:
// the victim must never be enqueued merely to be culled.
static void child_park_main(uint64_t boot_ch) {
  volatile uint32_t *ready = (volatile uint32_t *)boot_ch;
  __atomic_store_n(ready, 1, __ATOMIC_RELEASE);
  sys_block_doorbell(boot_ch);
  sys_block_wait(ready, 1);
  print("parked victim: RETURNED AFTER KILL\n");
  sys_proc_exit(0);
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

static void child_process_exit_main(uint64_t arg) {
  (void)arg;
  uint64_t stack = sys_vm_alloc(8192, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t stack_bytes = sys_vm_size(stack);
  sys_vm_protect(stack, 4096, 0);
  struct gdos_thread_start peer = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(peer),
      .entry = (uint64_t)child_burn_main,
      .stack_pointer =
          stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
  };
  uint64_t peer_tid = sys_thread_spawn(sys_getpid(), &peer);
  print("process-exit child: spinning peer tid=");
  print_hex(peer_tid);
  for (int i = 0; i < 64; i++) {
    sys_yield();
  }
  sys_proc_exit(0x42);
}

// Build a child process the parent-driven way and return its pid.
// entry runs on our shared image; boot_ch (may be 0) is its argument.
static uint64_t spawn_child(void (*entry)(uint64_t), uint64_t boot_ch) {
  uint64_t pid = sys_proc_create();
  print("tests: proc_create pid=");
  print_hex(pid);

  // Stack: built here, ownership transferred into the embryo.
  uint64_t stack = sys_vm_alloc(8192, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t stack_bytes = sys_vm_size(stack);
  print("tests: vm_move(stack) rc=");
  print_hex(sys_vm_move(stack, pid));
  print("tests: stack guard rc=");
  print_hex(sys_vm_protect_for(stack, 4096, 0, pid));
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

  struct gdos_thread_start start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(start),
      .entry = (uint64_t)entry,
      .argument = boot_ch,
      .stack_pointer =
          stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
  };
  uint64_t tid = sys_thread_spawn(pid, &start);
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
static uint64_t await_child_death(struct kring *tch) {
  struct kcqe cqe = {0};
  kring_wait_cqe(tch, &cqe);
  print(cqe.type == KEV_CHILD_DEAD ? "tests: KEV_CHILD_DEAD pid="
                                   : "tests: BAD TREE EVENT pid=");
  print_hex(cqe.a);
  kring_ack(tch); // consumption ack
  return cqe.b;
}

// A second thread proves that an owned, unshared block is a local event:
// the durable sequence closes an early-wake race, while the doorbell wakes
// the owner-side waiter when the first thread is already parked.
struct lifecycle_event {
  _Atomic uint32_t complete;
  _Atomic uint32_t release;
  uint64_t tid;
  uint64_t fs_marker;
  uint64_t tls_initial;
  uint64_t tls_zero;
  uint64_t tls_after_yield;
};

static void lifecycle_worker(uint64_t event_base) {
  struct lifecycle_event *event = (void *)event_base;
  event->tid = sys_gettid();
  __asm__ volatile("movq %%fs:0, %0" : "=r"(event->fs_marker));
  event->tls_initial = g_tls_initialized;
  event->tls_zero = g_tls_zero[2];
  g_tls_initialized = 0x574f524b4552544cull;
  sys_yield();
  event->tls_after_yield = g_tls_initialized;
  sys_block_wait(&event->release, 0);
  sys_thread_exit();
}

static void test_thread_lifecycle(void) {
  uint64_t main_tid = sys_gettid();
  uint64_t stack = sys_vm_alloc(8192, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t stack_bytes = sys_vm_size(stack);
  uint64_t tls = pe_tls_create((uint64_t)&__ImageBase);
  uint64_t fs = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t event_base = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(stack) || sys_iserr(stack_bytes) || tls == 0 ||
      sys_iserr(fs) || sys_iserr(event_base)) {
    print("tests: THREAD LIFECYCLE ALLOCATION FAILED\n");
    return;
  }
  *(uint64_t *)fs = 0x4653544c53424153ull;
  struct lifecycle_event *event = (void *)event_base;
  uint64_t guard_rc = sys_vm_protect(stack, 4096, 0);
  struct gdos_thread_start start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(start),
      .entry = (uint64_t)lifecycle_worker,
      .argument = event_base,
      .stack_pointer =
          stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
      .fs_base = fs,
      .gs_base = tls,
      .completion_event = (uint64_t)&event->complete,
  };
  uint64_t tid = sys_thread_spawn(sys_getpid(), &start);
  uint64_t pinned_free_rc = sys_vm_free(event_base);
  print(guard_rc == 0 &&
                sys_debug_write((const void *)stack, 1) == SYSERR_FAULT
            ? "tests: userspace stack guard ok\n"
            : "tests: USERSPACE STACK GUARD FAILED\n");
  print(pinned_free_rc == SYSERR_EXIST
            ? "tests: completion block pinned until exit\n"
            : "tests: COMPLETION PIN FAILED\n");

  atomic_store_explicit(&event->release, 1, memory_order_release);
  sys_block_doorbell(event_base);
  while (atomic_load_explicit(&event->complete, memory_order_acquire) == 0) {
    sys_block_wait(&event->complete, 0);
  }

  bool state_ok = tid == event->tid && tid != main_tid &&
                  main_tid == sys_gettid() &&
                  event->fs_marker == 0x4653544c53424153ull &&
                  event->tls_initial == 0x544c53494e495431ull &&
                  event->tls_zero == 0 &&
                  event->tls_after_yield == 0x574f524b4552544cull &&
                  g_tls_initialized == 0x4d41494e544c5331ull;
  print(state_ok ? "tests: tid and per-thread PE TLS ok\n"
                 : "tests: TID/TLS LIFECYCLE FAILED\n");

  bool reclaim_ok = sys_vm_free(stack) == 0 && sys_vm_free(tls) == 0 &&
                    sys_vm_free(fs) == 0 && sys_vm_free(event_base) == 0;
  print(reclaim_ok ? "tests: post-deschedule stack/TLS reclaim ok\n"
                   : "tests: POST-DESCHEDULE RECLAIM FAILED\n");
}

static bool timer_take(struct kring *timer, struct kcqe *out) {
  uint64_t rc = kring_wait_cqe(timer, out);
  if (rc != 0) {
    print("tests: TIMER WAIT FAILED rc=");
    print_hex(rc);
    return false;
  }
  kring_ack(timer);
  return true;
}

static bool timer_command(struct kring *timer, uint64_t op,
                          struct kcqe *out) {
  struct kcqe cqe;
  if (!timer_take(timer, &cqe))
    return false;
  if (cqe.type != op) {
    print("tests: TIMER COMMAND ORDER FAILED type=");
    print_hex(cqe.type);
    return false;
  }
  if (out != nullptr)
    *out = cqe;
  return true;
}

static void test_timer_scheme(void) {
  struct kring timer;
  uint64_t rc = kring_create(&timer, KSCHEME_TIMER, 4096);
  if (rc != 0) {
    print("tests: TIMER CREATE FAILED rc=");
    print_hex(rc);
    return;
  }

  struct kcqe cqe = {0};
  bool ok = kring_timer_now(&timer) == 0 &&
            timer_command(&timer, KTIMER_NOW, &cqe) && cqe.status == 0;
  uint64_t before = cqe.a;

  // The thread blocks with no polling. On an otherwise-idle owner CPU the
  // timer must remain armed even though the scheduling quantum is removed.
  uint64_t deadline = before + 5 * 1000 * 1000;
  ok &= kring_timer_arm_abs(&timer, 1, deadline, 0x54494d455231ull) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;
  ok &= timer_take(&timer, &cqe) && cqe.type == KEV_TIMER && cqe.a == 1 &&
        cqe.b == 0x54494d455231ull;

  ok &= kring_timer_now(&timer) == 0 &&
        timer_command(&timer, KTIMER_NOW, &cqe) && cqe.status == 0 &&
        cqe.a >= deadline && cqe.a >= before;
  uint64_t now = cqe.a;

  // Equal deadlines are distinct in the deadline tree and expire in arm
  // order (the tree's sequence-number tiebreaker).
  uint64_t shared_deadline = now + 20 * 1000 * 1000;
  ok &= kring_timer_arm_abs(&timer, 5, shared_deadline, 0x55) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;
  ok &= kring_timer_arm_abs(&timer, 6, shared_deadline, 0x66) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;
  ok &= timer_take(&timer, &cqe) && cqe.type == KEV_TIMER && cqe.a == 5 &&
        cqe.b == 0x55;
  ok &= timer_take(&timer, &cqe) && cqe.type == KEV_TIMER && cqe.a == 6 &&
        cqe.b == 0x66;

  // Duplicate live ids are rejected. Cancellation removes the original and
  // endpoint teardown below must also cancel a different outstanding timer.
  ok &= kring_timer_arm_abs(&timer, 2, now + 1000000000ull, 0x22) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;
  ok &= kring_timer_arm_abs(&timer, 2, now + 2000000000ull, 0x23) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) &&
        cqe.status == SYSERR_EXIST;
  ok &= kring_timer_cancel(&timer, 2) == 0 &&
        timer_command(&timer, KTIMER_CANCEL, &cqe) && cqe.status == 0;
  ok &= kring_timer_arm_abs(&timer, 3, now + 2000000000ull, 0x33) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;

  // Fill another timer CQ with command completions before its deadline. The
  // expiration must move to durable pending state and replay on our ack.
  ok &= kring_timer_now(&timer) == 0 &&
        timer_command(&timer, KTIMER_NOW, &cqe) && cqe.status == 0;
  uint64_t full_now = cqe.a;
  struct kring full;
  bool full_ok = kring_create(&full, KSCHEME_TIMER, 4096) == 0;
  if (full_ok) {
    full_ok &= kring_timer_arm_abs(&full, 9, full_now + 5000000ull, 0x99) ==
                   0 &&
               timer_command(&full, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;
    for (uint32_t i = 0; full_ok && i < full.nslots; i++) {
      struct ksqe *sqe = kring_get_sqe(&full);
      full_ok &= sqe != nullptr;
      if (sqe != nullptr)
        *sqe = (struct ksqe){.op = KTIMER_NOW};
    }
    full_ok &= kring_submit(&full) == 0;

    // A later timer on the ordinary ring gives the CQ-full expiration time to
    // occur without iteration-count timing or polling.
    full_ok &=
        kring_timer_arm_abs(&timer, 4, full_now + 20000000ull, 0x44) == 0 &&
        timer_command(&timer, KTIMER_ARM_ABS, &cqe) && cqe.status == 0;
    full_ok &= timer_take(&timer, &cqe) && cqe.type == KEV_TIMER &&
               cqe.a == 4 && cqe.b == 0x44;

    for (uint32_t i = 0; full_ok && i < full.nslots; i++) {
      const struct kcqe *queued = kring_peek_cqe(&full);
      full_ok &= queued != nullptr && queued->type == KTIMER_NOW &&
                 queued->status == 0;
      if (queued != nullptr)
        kring_cqe_seen(&full);
    }
    full_ok &= kring_ack(&full) == 0;
    full_ok &= timer_take(&full, &cqe) && cqe.type == KEV_TIMER &&
               cqe.a == 9 && cqe.b == 0x99;
    full_ok &= kring_destroy(&full) == 0;
  }
  ok &= full_ok;

  uint64_t destroy_rc = kring_destroy(&timer);
  ok &= destroy_rc == 0;
  print(ok ? "tests: monotonic timer scheme ok\n"
           : "tests: MONOTONIC TIMER SCHEME FAILED\n");
}

struct cpuid_result {
  uint32_t eax;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
};

static struct cpuid_result cpuid(uint32_t leaf, uint32_t subleaf) {
  struct cpuid_result r;
  __asm__ volatile("cpuid"
                   : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                   : "a"(leaf), "c"(subleaf));
  return r;
}

static uint64_t xgetbv0(void) {
  uint32_t lo;
  uint32_t hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return ((uint64_t)hi << 32) | lo;
}

static void test_arch_context(void) {
  print("tests: architecture context test starting\n");
  uint64_t fs = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t gs = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(fs) || sys_iserr(gs)) {
    print("tests: FS/GS ALLOCATION FAILED\n");
    return;
  }
  const uint64_t fs_value = 0x4653424153454f4bull;
  const uint64_t gs_value = 0x4753424153454f4bull;
  *(uint64_t *)fs = fs_value;
  *(uint64_t *)gs = gs_value;
  uint64_t bases_rc = sys_thread_bases_set(fs, gs);
  if (bases_rc != 0) {
    print("tests: FSBASE/GSBASE set rc=");
    print_hex(bases_rc);
  }
  sys_yield();
  uint64_t got_fs;
  uint64_t got_gs;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(got_fs));
  __asm__ volatile("movq %%gs:0, %0" : "=r"(got_gs));
  uint64_t reset_rc = sys_thread_bases_set(0, 0);
  print(bases_rc == 0 && reset_rc == 0 && got_fs == fs_value &&
                got_gs == gs_value
            ? "tests: FSBASE/GSBASE survive context switch\n"
            : "tests: FSBASE/GSBASE PRESERVATION FAILED\n");
  sys_vm_free(fs);
  sys_vm_free(gs);

  struct cpuid_result features = cpuid(1, 0);
  bool avx_enabled = (features.ecx & (1u << 27)) != 0 &&
                     (features.ecx & (1u << 28)) != 0 &&
                     (xgetbv0() & 0x6) == 0x6;
  if (!avx_enabled) {
    print("tests: AVX xstate test skipped\n");
    return;
  }

  const uint64_t ymm_value = 0x59534d4d53544154ull;
  uint64_t got_ymm;
  // Put the marker only in YMM0's upper 128 bits. FXSAVE would discard it;
  // eager XSAVE/XRSTOR must retain it across SYS_YIELD's park/resume path.
  __asm__ volatile("vmovq %0, %%xmm0\n\t"
                   "vinsertf128 $1, %%xmm0, %%ymm0, %%ymm0"
                   :
                   : "r"(ymm_value));
  sys_yield();
  __asm__ volatile("vextractf128 $1, %%ymm0, %%xmm1\n\t"
                   "vmovq %%xmm1, %0\n\t"
                   "vzeroupper"
                   : "=r"(got_ymm));
  print(got_ymm == ymm_value ? "tests: AVX xstate survives context switch\n"
                             : "tests: AVX XSTATE PRESERVATION FAILED\n");
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

  // --- Child 4: kill a thread parked in a channel ----------------------
  uint64_t park_ch = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  volatile uint32_t *ready = (volatile uint32_t *)park_ch;
  uint64_t c4 = spawn_child(child_park_main, park_ch);
  sys_block_wait(ready, 0);
  for (int i = 0; i < 64; i++) {
    sys_yield();
  }
  print("tests: proc_kill(parked) rc=");
  print_hex(sys_proc_kill(c4));
  await_child_death(tch);
  // Revocation clears the dead child's waiter slot without enqueueing it;
  // bounded reap subsequently owns the detached blocked TCB.
  print("tests: vm_free(park_ch) rc=");
  print_hex(sys_vm_free(park_ch));
  reap_child(c4);

  // --- Child 5: one thread exits the whole multithreaded process ----------
  uint64_t c5 = spawn_child(child_process_exit_main, 0);
  uint64_t exit_status = await_child_death(tch);
  print(exit_status == 0x42 ? "tests: process-wide exit status/peer cull ok\n"
                            : "tests: PROCESS-WIDE EXIT FAILED\n");
  reap_child(c5);

}

void _start(uint64_t arg) {
  (void)arg;
  bool initial_tls_ok = g_tls_initialized == 0x544c53494e495431ull &&
                        g_tls_zero[0] == 0 && g_tls_zero[3] == 0;
  print("tests: hello from the ring-3 suite (a real process, not init)\n");
  print(initial_tls_ok ? "tests: loader installed PE TLS before entry\n"
                       : "tests: INITIAL PE TLS FAILED\n");
  g_tls_initialized = 0x4d41494e544c5331ull;
  // If the userland loader (gdoslib-dev/pe.c) relocated us wrong, this jumps
  // into the weeds right here.
  g_reloc_fn(g_reloc_str);

  print("tests: pid=");
  print_hex(sys_getpid());

  sys_yield();
  print("tests: back from yield\n");

  test_memory();
  test_realloc();
  test_thread_lifecycle();
  test_timer_scheme();
  test_arch_context();

  // Tree channel: our children's deaths arrive here (we are a mid-tree
  // process now — init watches for OUR death the same way).
  struct kring tch;
  print("tests: kring_create(tch, -3) rc=");
  print_hex(kring_create(&tch, KSCHEME_TREE, 4096));
  test_process_tree(&tch);

  print("tests: all tests done\n");
  // Unlike the old hello.c-as-init, this process may exit: init reaps
  // us, which frees our image, stack, and the tree-channel block.
  sys_proc_exit(0);
}
