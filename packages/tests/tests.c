// The ring-3 test suite for govindos, shipped in the initfs and spawned
// by init through the userland PE loader (gdoslib-dev/pe.c) as a real
// separate process — which makes every boot regression-test that loader
// too. Descended from the original hello.c, which doubled as init and
// the suite until the boot-init design split the roles
// (docs/technical/boot-init-design.md §0).
//
// Children below are built here, parent-driven, the way real userspace
// does it (ipc-process-design.md §5): PROC_CREATE a child, VM_MOVE it
// a stack, VM_SHARE it this very image read-execute (SASOS: the child
// runs the same code at the same addresses), pre-seed a bootstrap
// channel, THREAD_SPAWN its first thread. Children exercise the channel
// data plane from the far side; kill/destroy/tree-events are exercised from
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
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
  print("pe: vm_dropshare of owned block rc=");
  print_hex(sys_vm_dropshare(blk, 0));
  print("pe: vm_unshare with no edge rc=");
  print_hex(sys_vm_unshare(blk, 0xdead));
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

static int compare_ints(const void *left, const void *right) {
  int a = *(const int *)left;
  int b = *(const int *)right;
  return (a > b) - (a < b);
}

static void test_libc_surface(void) {
  char buffer[32] = "govind";
  char tokens[] = "one,two,three";
  char *save = nullptr;
  int values[] = {7, 1, 9, 3, 2};
  int key = 3;
  char *end = nullptr;

  strcat(buffer, "os");
  qsort(values, sizeof(values) / sizeof(values[0]), sizeof(values[0]),
        compare_ints);
  int *found = bsearch(&key, values, sizeof(values) / sizeof(values[0]),
                       sizeof(values[0]), compare_ints);
  errno = 0;
  long overflow = strtol("2147483648tail", &end, 10);
  char *copy = strdup(buffer);
  bool ok = strcmp(buffer, "govindos") == 0 &&
            strcasecmp("GovindOS", "govindos") == 0 &&
            strstr("small libc surface", "libc") != nullptr &&
            strtok_r(tokens, ",", &save) == tokens &&
            strcmp(strtok_r(nullptr, ",", &save), "two") == 0 &&
            isalpha('Q') && isdigit('7') && tolower('Z') == 'z' &&
            values[0] == 1 && values[4] == 9 && found != nullptr &&
            *found == 3 && overflow == LONG_MAX && errno == ERANGE &&
            strcmp(end, "tail") == 0 && copy != nullptr &&
            strcmp(copy, "govindos") == 0;
  free(copy);
  print(ok ? "tests: basic libc surface ok\n"
           : "tests: BASIC LIBC SURFACE FAILED\n");
}

static pthread_mutex_t g_pthread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pthread_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_pthread_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_pthread_key;
static _Atomic uint32_t g_pthread_ready;
static _Atomic uint32_t g_pthread_once_calls;
static _Atomic uint32_t g_pthread_destructors;
static _Atomic uint32_t g_pthread_failures;
static _Atomic uint32_t g_detached_done;
static int g_pthread_sum;
static _Thread_local uint64_t g_pthread_tls = 0x5054485245414454ull;

static void pthread_once_probe(void) {
  atomic_fetch_add_explicit(&g_pthread_once_calls, 1, memory_order_relaxed);
}

static void pthread_destructor_probe(void *value) {
  if (value == nullptr)
    atomic_fetch_add_explicit(&g_pthread_failures, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_pthread_destructors, 1,
                            memory_order_relaxed);
}

static void *pthread_worker(void *argument) {
  uint32_t id = (uint32_t)(uintptr_t)argument;
  pthread_t self = pthread_self();
  if (self == nullptr || !pthread_equal(self, pthread_self()) ||
      g_pthread_tls != 0x5054485245414454ull)
    atomic_fetch_add_explicit(&g_pthread_failures, 1, memory_order_relaxed);
  g_pthread_tls = id;
  if (pthread_once(&g_pthread_once, pthread_once_probe) != 0 ||
      pthread_setspecific(g_pthread_key, (void *)(uintptr_t)id) != 0)
    atomic_fetch_add_explicit(&g_pthread_failures, 1, memory_order_relaxed);

  pthread_mutex_lock(&g_pthread_mutex);
  g_pthread_sum += (int)id;
  atomic_fetch_add_explicit(&g_pthread_ready, 1, memory_order_release);
  pthread_cond_broadcast(&g_pthread_cond);
  pthread_mutex_unlock(&g_pthread_mutex);
  sys_yield();
  if (g_pthread_tls != id ||
      pthread_getspecific(g_pthread_key) != (void *)(uintptr_t)id)
    atomic_fetch_add_explicit(&g_pthread_failures, 1, memory_order_relaxed);
  return (void *)(uintptr_t)(0x100u + id);
}

static void *detached_worker(void *argument) {
  (void)argument;
  atomic_store_explicit(&g_detached_done, 1, memory_order_release);
  return nullptr;
}

static void test_pthreads(void) {
  pthread_t threads[2] = {nullptr, nullptr};
  void *results[2] = {nullptr, nullptr};
  bool ok = pthread_key_create(&g_pthread_key, pthread_destructor_probe) == 0;
  unsigned created = 0;
  for (unsigned i = 0; i < 2; i++) {
    if (pthread_create(&threads[i], nullptr, pthread_worker,
                       (void *)(uintptr_t)(i + 1)) == 0)
      created++;
  }

  pthread_mutex_lock(&g_pthread_mutex);
  while (atomic_load_explicit(&g_pthread_ready, memory_order_acquire) <
         created)
    pthread_cond_wait(&g_pthread_cond, &g_pthread_mutex);
  pthread_mutex_unlock(&g_pthread_mutex);

  for (unsigned i = 0; i < 2; i++) {
    if (threads[i] != nullptr)
      ok &= pthread_join(threads[i], &results[i]) == 0;
  }
  ok &= created == 2 && results[0] == (void *)(uintptr_t)0x101 &&
        results[1] == (void *)(uintptr_t)0x102 && g_pthread_sum == 3 &&
        atomic_load_explicit(&g_pthread_once_calls, memory_order_relaxed) ==
            1 &&
        atomic_load_explicit(&g_pthread_destructors, memory_order_relaxed) ==
            2 &&
        atomic_load_explicit(&g_pthread_failures, memory_order_relaxed) == 0 &&
        g_pthread_tls == 0x5054485245414454ull;
  pthread_key_delete(g_pthread_key);

  pthread_mutexattr_t mutex_attr;
  pthread_mutex_t recursive;
  ok &= pthread_mutexattr_init(&mutex_attr) == 0 &&
        pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&recursive, &mutex_attr) == 0 &&
        pthread_mutex_lock(&recursive) == 0 &&
        pthread_mutex_lock(&recursive) == 0 &&
        pthread_mutex_unlock(&recursive) == 0 &&
        pthread_mutex_unlock(&recursive) == 0 &&
        pthread_mutex_destroy(&recursive) == 0;
  pthread_mutexattr_destroy(&mutex_attr);

  pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
  pthread_spinlock_t spin = 0;
  pthread_barrier_t barrier;
  ok &= pthread_rwlock_rdlock(&rwlock) == 0 &&
        pthread_rwlock_unlock(&rwlock) == 0 &&
        pthread_rwlock_wrlock(&rwlock) == 0 &&
        pthread_rwlock_unlock(&rwlock) == 0 &&
        pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) == 0 &&
        pthread_spin_lock(&spin) == 0 && pthread_spin_unlock(&spin) == 0 &&
        pthread_barrier_init(&barrier, nullptr, 1) == 0 &&
        pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD &&
        pthread_barrier_destroy(&barrier) == 0;

  pthread_attr_t detached_attr;
  pthread_t detached = nullptr;
  ok &= pthread_attr_init(&detached_attr) == 0 &&
        pthread_attr_setdetachstate(&detached_attr,
                                    PTHREAD_CREATE_DETACHED) == 0 &&
        pthread_create(&detached, &detached_attr, detached_worker, nullptr) ==
            0;
  pthread_attr_destroy(&detached_attr);
  while (ok &&
         atomic_load_explicit(&g_detached_done, memory_order_acquire) == 0)
    sys_yield();
  for (unsigned i = 0; i < 8; i++)
    sys_yield();

  // Timed waits route through the futex deadline: a never-signalled
  // condvar times out, and a free mutex timedlocks without parking even
  // when the deadline has already passed.
  pthread_mutex_t timed_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t timed_cond = PTHREAD_COND_INITIALIZER;
  struct timespec abstime;
  ok &= clock_gettime(CLOCK_MONOTONIC, &abstime) == 0;
  abstime.tv_nsec += 3000000;
  if (abstime.tv_nsec >= 1000000000L) {
    abstime.tv_sec++;
    abstime.tv_nsec -= 1000000000L;
  }
  ok &= pthread_mutex_lock(&timed_mutex) == 0 &&
        pthread_cond_timedwait(&timed_cond, &timed_mutex, &abstime) ==
            ETIMEDOUT &&
        pthread_mutex_unlock(&timed_mutex) == 0 &&
        pthread_mutex_timedlock(&timed_mutex, &abstime) == 0 &&
        pthread_mutex_unlock(&timed_mutex) == 0;
  print(ok ? "tests: pthread lifecycle/TLS/synchronization ok\n"
           : "tests: PTHREAD IMPLEMENTATION FAILED\n");
}

#define FUTEX_WAITER_COUNT (FUTEX_WAKE_BATCH + 4)

struct futex_waiter_arg {
  uint64_t word;
  uint64_t deadline;
  uint64_t result;
};

static _Atomic uint32_t g_futex_ready;
static struct futex_waiter_arg g_futex_args[FUTEX_WAITER_COUNT];

static void *futex_waiter(void *argument) {
  struct futex_waiter_arg *arg = argument;
  atomic_fetch_add_explicit(&g_futex_ready, 1, memory_order_release);
  arg->result = sys_futex_wait((const volatile void *)arg->word, 0, nullptr,
                               arg->deadline);
  return nullptr;
}

// Direct exercise of the futex surface: compare mismatch, deadline
// expiry, view gating, count-capped FIFO wakes past the batch bound,
// requeue, and the no-notification revocation story (waiters parked
// across VM_FREE recover via their deadlines; the view check reports
// SYSERR_INVAL afterwards).
static void test_futex(void) {
  uint64_t blk = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  bool ok = !sys_iserr(blk);
  volatile uint32_t *word = (volatile uint32_t *)blk;

  // The word is compared, never interpreted: a mismatch reports AGAIN.
  ok &= sys_futex_wait(word, 123, nullptr, 0) == SYSERR_AGAIN;
  // A deadline already behind us parks and promptly times out.
  ok &= sys_futex_wait(word, 0, nullptr, sys_gettime() + 1) ==
        SYSERR_TIMEDOUT;
  // The wake and wait namespaces are gated by view membership.
  ok &= sys_futex_wait((const volatile void *)0x1000, 0, nullptr, 0) ==
        SYSERR_INVAL;
  ok &= sys_iserr(sys_futex_wake((const volatile void *)0x1000, 1));
  // Waking a word nobody waits on wakes nobody.
  ok &= sys_futex_wake(word, UINT64_MAX) == 0;

  // More waiters than one wake batch: the count-capped wake returns its
  // count and the caller loops.
  pthread_t threads[FUTEX_WAITER_COUNT] = {0};
  unsigned created = 0;
  for (unsigned i = 0; ok && i < FUTEX_WAITER_COUNT; i++) {
    g_futex_args[i] = (struct futex_waiter_arg){.word = blk};
    if (pthread_create(&threads[i], nullptr, futex_waiter,
                       &g_futex_args[i]) != 0)
      break;
    created++;
  }
  ok &= created == FUTEX_WAITER_COUNT;
  while (atomic_load_explicit(&g_futex_ready, memory_order_acquire) !=
         created)
    sys_yield();
  uint64_t woken = 0;
  unsigned batches = 0;
  for (unsigned spin = 0; woken < created && spin < 100000; spin++) {
    uint64_t rc = sys_futex_wake(word, UINT64_MAX);
    if (sys_iserr(rc)) {
      break;
    }
    woken += rc;
    if (rc != 0)
      batches++;
    else
      sys_yield(); // a ready worker may not have parked yet
  }
  ok &= woken == created && batches >= 2;
  for (unsigned i = 0; i < created; i++) {
    ok &= pthread_join(threads[i], nullptr) == 0;
    ok &= g_futex_args[i].result == 0;
  }

  // Requeue: waiters move from one word to another without waking, keep
  // FIFO order, and the compare guard rejects a stale expected value.
  volatile uint32_t *to = (volatile uint32_t *)(blk + 64);
  pthread_t movers[4] = {0};
  atomic_store_explicit(&g_futex_ready, 0, memory_order_relaxed);
  for (unsigned i = 0; ok && i < 4; i++) {
    g_futex_args[i] = (struct futex_waiter_arg){.word = blk};
    ok &= pthread_create(&movers[i], nullptr, futex_waiter,
                         &g_futex_args[i]) == 0;
  }
  while (atomic_load_explicit(&g_futex_ready, memory_order_acquire) != 4)
    sys_yield();
  ok &= sys_futex_requeue(word, to, 999, UINT64_MAX) == SYSERR_AGAIN;
  uint64_t moved = 0;
  for (unsigned spin = 0; moved < 4 && spin < 100000; spin++) {
    uint64_t rc = sys_futex_requeue(word, to, 0, UINT64_MAX);
    if (sys_iserr(rc))
      break;
    moved += rc;
    if (rc == 0)
      sys_yield();
  }
  ok &= moved == 4;
  ok &= sys_futex_wake(word, UINT64_MAX) == 0; // nobody left on `from`
  woken = 0;
  for (unsigned spin = 0; woken < 4 && spin < 100000; spin++) {
    uint64_t rc = sys_futex_wake(to, UINT64_MAX);
    if (sys_iserr(rc))
      break;
    woken += rc;
    if (rc == 0)
      sys_yield();
  }
  ok &= woken == 4;
  for (unsigned i = 0; i < 4; i++) {
    ok &= pthread_join(movers[i], nullptr) == 0 &&
          g_futex_args[i].result == 0;
  }

  // Revocation notifies nobody: waiters parked across the free recover
  // via their deadlines, and the freed address stops being waitable.
  pthread_t parked[2] = {0};
  atomic_store_explicit(&g_futex_ready, 0, memory_order_relaxed);
  uint64_t deadline = sys_gettime() + 50ull * 1000 * 1000;
  for (unsigned i = 0; ok && i < 2; i++) {
    g_futex_args[i] =
        (struct futex_waiter_arg){.word = blk, .deadline = deadline};
    ok &= pthread_create(&parked[i], nullptr, futex_waiter,
                         &g_futex_args[i]) == 0;
  }
  while (atomic_load_explicit(&g_futex_ready, memory_order_acquire) != 2)
    sys_yield();
  for (unsigned i = 0; i < 64; i++)
    sys_yield(); // let both actually park
  ok &= sys_vm_free(blk) == 0 && sys_vm_size(blk) == SYSERR_PERM;
  for (unsigned i = 0; i < 2; i++) {
    ok &= pthread_join(parked[i], nullptr) == 0 &&
          g_futex_args[i].result == SYSERR_TIMEDOUT;
  }
  ok &= sys_futex_wait(word, 0, nullptr, 0) == SYSERR_INVAL;

  print(ok ? "tests: futex wait/wake/requeue/timeout ok\n"
           : "tests: FUTEX SURFACE FAILED\n");
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
#define CH_REQ_CLOSE 2u // owner's close sentinel in the req word

// First thread of the served child. Runs on init's image (read-execute
// view — stack locals only!) with the bootstrap-channel base as its
// argument. Proves the whole far side of the establishment flow: the
// shares channel replays the pre-spawn seed share, the data plane works
// both ways, and revocation wakes a parked waiter with SYSERR_DEAD.
static void child_main(uint64_t boot_ch) {
  print("child: hello (spawned by tests)\n");

  // Create a shares channel; the bootstrap block was shared to us while
  // it predated this ring, so its KEV_SHARE must replay right here
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
  // Both peers name the words they park on — role inference is gone.
  volatile uint32_t *req = (volatile uint32_t *)(boot_ch + CH_REQ_OFF);
  volatile uint32_t *resp = (volatile uint32_t *)(boot_ch + CH_RESP_OFF);
  volatile char *msg = (volatile char *)(boot_ch + CH_MSG_OFF);
  sys_futex_wait(req, 0, nullptr, 0);
  while (__atomic_load_n(req, __ATOMIC_ACQUIRE) == 0) {
    sys_yield();
  }
  for (uint64_t i = 0; i < CH_MSG_MAX && msg[i] != '\0'; i++) {
    if (msg[i] >= 'a' && msg[i] <= 'z') {
      msg[i] = (char)(msg[i] - 'a' + 'A');
    }
  }
  __atomic_store_n(resp, 1, __ATOMIC_RELEASE);
  sys_futex_wake(resp, 1);

  // Orderly teardown (futex-design.md §5): park until the owner writes
  // the close sentinel and wakes us. Our VM_DROPSHARE is the ack that
  // lets the owner's VM_FREE succeed; after it we never touch the block.
  uint64_t rc = sys_futex_wait(req, 1, nullptr, 0);
  bool sentinel = (rc == 0 || rc == SYSERR_AGAIN) &&
                  __atomic_load_n(req, __ATOMIC_ACQUIRE) == CH_REQ_CLOSE;
  print(sentinel ? "child: close sentinel ok, dropping share\n"
                 : "child: CLOSE SENTINEL WRONG\n");
  print("child: vm_dropshare(boot_ch) rc=");
  print_hex(sys_vm_dropshare(boot_ch, 0));
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
// process exercises lazy dead-waiter detachment and explicit TCB disposal:
// the victim must never be enqueued merely to be culled.
static void child_park_main(uint64_t boot_ch) {
  volatile uint32_t *ready = (volatile uint32_t *)boot_ch;
  __atomic_store_n(ready, 1, __ATOMIC_RELEASE);
  sys_futex_wake(ready, 1);
  sys_futex_wait(ready, 1, nullptr, 0);
  print("parked victim: RETURNED AFTER KILL\n");
  sys_proc_exit(0);
}

// First thread of the preemption-test child: burns CPU in ring 3 and
// never enters the kernel again. Only the quantum timer can pull it in,
// so killing AND destroying it proves preemption end to end — destruction
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

struct parent_authority_event {
  _Atomic uint32_t primary_ready;
  _Atomic uint32_t worker_ran;
  _Atomic uint32_t release;
};

static void child_parent_managed_main(uint64_t event_base) {
  struct parent_authority_event *event = (void *)event_base;
  atomic_store_explicit(&event->primary_ready, 1, memory_order_release);
  sys_futex_wake(&event->primary_ready, 1);
  while (atomic_load_explicit(&event->release, memory_order_acquire) == 0)
    sys_futex_wait(&event->release, 0, nullptr, 0);
  sys_proc_exit(0);
}

static void child_parent_spawned_worker(uint64_t event_base) {
  struct parent_authority_event *event = (void *)event_base;
  atomic_store_explicit(&event->worker_ran, 1, memory_order_release);
  sys_futex_wake(&event->worker_ran, 1);
  sys_thread_exit();
}

// Build a child process the parent-driven way and return its pid.
// entry runs on our shared image; boot_ch (may be 0) is its argument.
static uint64_t spawn_child(void (*entry)(uint64_t), uint64_t boot_ch) {
  uint64_t pid = sys_proc_create();
  print("tests: proc_create pid=");
  print_hex(pid);

  // Stack: built here, ownership transferred into the child.
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
    // Pre-seed the bootstrap channel before starting the child — the
    // seL4/Xen answer to how strangers are initially introduced.
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

static void destroy_child(uint64_t pid) {
  print(pe_destroy(pid) ? "tests: process destroyed\n"
                        : "tests: PROCESS DESTROY FAILED\n");
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
  sys_futex_wait(&event->release, 0, nullptr, 0);
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
  sys_futex_wake(&event->release, 1);
  while (atomic_load_explicit(&event->complete, memory_order_acquire) == 0) {
    sys_futex_wait(&event->complete, 0, nullptr, 0);
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

// The timer scheme is gone (timer-design.md): SYS_GETTIME is the clock
// and every timed wait is a futex deadline. The thread blocks with no
// polling; on an otherwise-idle CPU the deadline must stay armed even
// though the scheduling quantum is removed.
static void test_time(void) {
  uint64_t t0 = sys_gettime();
  uint64_t t1 = sys_gettime();
  bool ok = t0 != 0 && t1 >= t0;

  uint32_t parked_word = 0;
  uint64_t deadline = t1 + 5ull * 1000 * 1000;
  uint64_t rc = sys_futex_wait(&parked_word, 0, nullptr, deadline);
  uint64_t after = sys_gettime();
  ok &= rc == SYSERR_TIMEDOUT && after >= deadline;

  // libc surface over the same mechanism.
  struct timespec req = {.tv_sec = 0, .tv_nsec = 2000000};
  uint64_t before_sleep = sys_gettime();
  ok &= nanosleep(&req, nullptr) == 0 &&
        sys_gettime() >= before_sleep + 2000000ull;
  struct timespec now;
  ok &= clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
        (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec >=
            before_sleep;

  print(ok ? "tests: gettime + futex deadline sleep ok\n"
           : "tests: TIME/DEADLINE FAILED\n");
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
  sys_futex_wake(req, 1);
  sys_futex_wait(resp, 0, nullptr, 0);
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
  // the woken-from-park path, not just the racing one) ...
  for (int i = 0; i < 64; i++) {
    sys_yield();
  }
  // ... then run the orderly teardown choreography (futex-design.md §5):
  // close sentinel, wake, the child's VM_DROPSHARE is the ack, and
  // VM_FREE is the drain gate — SYSERR_EXIST until the share edge drains.
  __atomic_store_n(req, CH_REQ_CLOSE, __ATOMIC_RELEASE);
  sys_futex_wake(req, 1);
  uint64_t free_rc;
  while ((free_rc = sys_vm_free(boot_ch)) == SYSERR_EXIST) {
    sys_yield();
  }
  print("tests: vm_free(boot_ch) after dropshare rc=");
  print_hex(free_rc);
  await_child_death(tch);
  destroy_child(c1);

  // A destroyed pid is gone for good (never reused): all verbs refuse it.
  print("tests: destroy of destroyed pid rc=");
  print_hex(sys_proc_destroy(c1));

  // --- Child 2: direct-parent authority remains after first spawn -------
  uint64_t authority_block =
      sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  struct parent_authority_event *authority = (void *)authority_block;
  uint64_t c2 = spawn_child(child_parent_managed_main, authority_block);
  while (atomic_load_explicit(&authority->primary_ready,
                              memory_order_acquire) == 0)
    sys_futex_wait(&authority->primary_ready, 0, nullptr, 0);

  uint64_t live_stack = sys_vm_alloc(8192, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t live_stack_bytes = sys_vm_size(live_stack);
  uint64_t live_move = sys_vm_move(live_stack, c2);
  uint64_t live_protect = sys_vm_protect_for(live_stack, 4096, 0, c2);
  struct gdos_thread_start live_start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(live_start),
      .entry = (uint64_t)child_parent_spawned_worker,
      .argument = authority_block,
      .stack_pointer =
          live_stack + live_stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
  };
  uint64_t live_tid = sys_thread_spawn(c2, &live_start);
  bool live_authority_ok =
      live_move == 0 && live_protect == 0 && !sys_iserr(live_tid);
  if (live_authority_ok) {
    while (atomic_load_explicit(&authority->worker_ran,
                                memory_order_acquire) == 0)
      sys_futex_wait(&authority->worker_ran, 0, nullptr, 0);
  }
  print(live_authority_ok &&
                atomic_load_explicit(&authority->worker_ran,
                                     memory_order_acquire) != 0
            ? "tests: live direct-child move/protect/spawn ok\n"
            : "tests: LIVE DIRECT-CHILD AUTHORITY FAILED\n");
  atomic_store_explicit(&authority->release, 1, memory_order_release);
  sys_futex_wake(&authority->release, 1);
  await_child_death(tch);
  print("tests: vm_unshare(parent-authority block) rc=");
  print_hex(sys_vm_unshare(authority_block, c2));
  destroy_child(c2);
  if (live_move != 0)
    sys_vm_free(live_stack);
  print("tests: vm_free(parent-authority block) rc=");
  print_hex(sys_vm_free(authority_block));

  // --- Child 3: kill a running process ----------------------------------
  uint64_t c3 = spawn_child(child_spin_main, 0);
  sys_yield(); // let the victim actually run
  print("tests: proc_kill rc=");
  print_hex(sys_proc_kill(c3));
  await_child_death(tch);
  destroy_child(c3);

  // Kill authority: only descendants (not self, not strangers).
  print("tests: kill self rc=");
  print_hex(sys_proc_kill(sys_getpid()));

  // --- Child 4: kill a CPU-bound process (preemption test) --------------
  // The burner never syscalls, so before timer preemption this could
  // never terminate: the victim would spin in ring 3 forever, its thread
  // never culled and its AS pinned on whatever CPU ran it.
  uint64_t c4 = spawn_child(child_burn_main, 0);
  for (int i = 0; i < 64; i++) {
    sys_yield(); // let the burner get dispatched somewhere
  }
  print("tests: proc_kill(burner) rc=");
  print_hex(sys_proc_kill(c4));
  await_child_death(tch);
  destroy_child(c4);

  // --- Child 5: kill a thread parked on a revoked block -----------------
  uint64_t park_ch = sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  volatile uint32_t *ready = (volatile uint32_t *)park_ch;
  uint64_t c5 = spawn_child(child_park_main, park_ch);
  while (__atomic_load_n(ready, __ATOMIC_ACQUIRE) == 0) {
    sys_futex_wait(ready, 0, nullptr, 0);
  }
  for (int i = 0; i < 64; i++) {
    sys_yield();
  }
  // Owner-side coercion: revoke the (unresponsive) peer's edge, then the
  // free succeeds with the peer's thread still parked on the word — the
  // kernel never wakes waiters on revocation.
  print("tests: vm_unshare(park_ch, c5) rc=");
  print_hex(sys_vm_unshare(park_ch, c5));
  print("tests: vm_free(park_ch) rc=");
  print_hex(sys_vm_free(park_ch));
  print("tests: proc_kill(parked) rc=");
  print_hex(sys_proc_kill(c5));
  await_child_death(tch);
  // Reap claims the parked TCB (PARKED -> CLAIMED), removes its futex
  // node, and frees it.
  destroy_child(c5);

  // --- Child 6: one thread exits the whole multithreaded process ----------
  uint64_t c6 = spawn_child(child_process_exit_main, 0);
  uint64_t exit_status = await_child_death(tch);
  print(exit_status == 0x42 ? "tests: process-wide exit status/peer cull ok\n"
                            : "tests: PROCESS-WIDE EXIT FAILED\n");
  destroy_child(c6);

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
  test_libc_surface();
  test_pthreads();
  test_futex();
  test_thread_lifecycle();
  test_time();
  test_arch_context();

  // Tree channel: our children's deaths arrive here (we are a mid-tree
  // process now — init watches for OUR death the same way).
  struct kring tch;
  print("tests: kring_create(tch, -3) rc=");
  print_hex(kring_create(&tch, KSCHEME_TREE, 4096));
  test_process_tree(&tch);

  print("tests: all tests done\n");
  // Unlike the old hello.c-as-init, this process may exit: init destroys
  // us, which frees our image, stack, and the tree-channel block.
  sys_proc_exit(0);
}
