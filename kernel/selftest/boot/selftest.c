#include "selftest.h"

#include <stdatomic.h>
#include <stdint.h>

#include "acpi.h"
#include "channel.h"
#include "debug.h"
#include "paging.h"
#include "process.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "uaccess.h"
#include "umem.h"

#include <gdosabi/syscall.h>
#include <siphash/siphash.h>

void siphash_selftest(void) {
  // Official SipHash-2-4 128-bit test vector: key 00..0f, input 00..0e.
  static const uint8_t vec_key[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                                      8, 9, 10, 11, 12, 13, 14, 15};
  static const uint8_t vec_in[15] = {0, 1, 2,  3,  4,  5,  6, 7,
                                     8, 9, 10, 11, 12, 13, 14};
  static const uint8_t vec_tag[16] = {
      0x54,0x93,0xe9,0x99,0x33,0xb0,0xa8,0x11,0x7e,0x08,0xec,0x0f,0x97,0xcf,
      0xc3,0xd9};
  uint8_t tag[16];
  siphash128(vec_in, sizeof(vec_in), vec_key, tag);
  for (uint32_t i = 0; i < sizeof(tag); i++)
    asserts(tag[i] == vec_tag[i], "siphash selftest: vector mismatch");
}

static uint32_t selftest_prng(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

// Exercise the strengthened owned-tree contract: arbitrary rotations and
// two-child removals must preserve every surviving inline value's address.
void llrb_identity_selftest(void) {
  enum { N = 193 };
  llrb_base_block *tree;
  asserts(llrb_base_block_new(&tree), "llrb selftest: tree alloc failed");
  uint16_t order[N];
  ublock *refs[N];
  bool present[N];
  for (uint32_t i = 0; i < N; i++) {
    order[i] = (uint16_t)i;
    present[i] = false;
  }
  uint32_t rng = 0xC0DEF00Du;
  for (uint32_t i = N - 1; i != 0; i--) {
    uint32_t j = selftest_prng(&rng) % (i + 1);
    uint16_t tmp = order[i];
    order[i] = order[j];
    order[j] = tmp;
  }
  for (uint32_t i = 0; i < N; i++) {
    uint64_t key = order[i] + 1;
    ublock value = {.base = key};
    asserts(llrb_base_block_insert(tree, &key, &value) &&
                llrb_base_block_get_ref(tree, &key, &refs[key - 1]) &&
                llrb_base_block_valid(tree),
            "llrb selftest: insert/valid failed");
    present[key - 1] = true;
  }
  for (uint32_t i = 0; i < N; i++) {
    uint64_t key = order[i] + 1;
    ublock old;
    llrb_base_block_node *node;
    asserts(llrb_base_block_extract(tree, &key, &old, &node) &&
                &node->value == refs[key - 1] &&
                llrb_base_block_valid(tree) &&
                llrb_base_block_insert_node(tree, &key, &node->value, node) &&
                llrb_base_block_valid(tree),
            "llrb selftest: identity relink failed");
    ublock *ref;
    asserts(llrb_base_block_get_ref(tree, &key, &ref) &&
                ref == refs[key - 1],
            "llrb selftest: value address changed");
  }
  for (uint32_t i = 0; i < N; i++) {
    uint64_t key = order[N - 1 - i] + 1;
    ublock old;
    asserts(llrb_base_block_remove(tree, &key, &old) &&
                llrb_base_block_valid(tree),
            "llrb selftest: remove/valid failed");
    present[key - 1] = false;
    for (uint32_t j = 0; j < N; j++) {
      if (!present[j])
        continue;
      uint64_t survivor = j + 1;
      ublock *ref;
      asserts(llrb_base_block_get_ref(tree, &survivor, &ref) &&
                  ref == refs[j],
              "llrb selftest: survivor address changed");
    }
  }
  llrb_base_block_delete(&tree);
  printf("llrb: randomized identity selftest ok\n");
}

// Synchronous implementation of the explicit teardown choreography for
// boot-only process fixtures. No fixture has threads or DMA mappings.
static void destroy_test_process(struct process *p) {
  process_kill_subtree(p);
  while (llrb_pid_process_len(p->children) != 0) {
    llrb_pid_process_iter iter;
    llrb_pid_process_iter_begin(p->children, &iter);
    struct process *child;
    asserts(llrb_pid_process_iter_next(&iter, nullptr, &child),
            "destroy_test_process: child tree empty");
    destroy_test_process(child);
  }
  while (llrb_base_edge_len(p->views) != 0) {
    llrb_base_edge_iter iter;
    llrb_base_edge_iter_begin(p->views, &iter);
    uint64_t base;
    asserts(llrb_base_edge_iter_next(&iter, &base, nullptr),
            "destroy_test_process: view tree empty");
    asserts(umem_dropshare(p, base, 0) == 0,
            "destroy_test_process: dropshare failed");
  }
  while (llrb_base_block_len(p->blocks) != 0) {
    llrb_base_block_iter blocks;
    llrb_base_block_iter_begin(p->blocks, &blocks);
    uint64_t base;
    ublock *b;
    asserts(llrb_base_block_iter_next(&blocks, &base, nullptr) &&
                llrb_base_block_get_ref(p->blocks, &base, &b),
            "destroy_test_process: block tree empty");
    while (llrb_pid_edge_len(b->sharers) != 0) {
      llrb_pid_edge_iter sharers;
      llrb_pid_edge_iter_begin(b->sharers, &sharers);
      uint64_t pid;
      asserts(llrb_pid_edge_iter_next(&sharers, &pid, nullptr),
              "destroy_test_process: sharer tree empty");
      asserts(umem_unshare(p, base, pid) == 0,
              "destroy_test_process: unshare failed");
    }
    asserts(umem_free(p, base) == 0,
            "destroy_test_process: free failed");
  }
  umem_lock();
  asserts(llrb_tid_thread_len(p->threads) == 0 &&
              llrb_pid_process_len(p->children) == 0,
          "destroy_test_process: process not empty");
  as_free(p->as);
  if (p->parent != nullptr) {
    struct process *removed;
    asserts(llrb_pid_process_remove(p->parent->children, &p->pid, &removed) &&
                removed == p,
            "destroy_test_process: child missing");
  }
  umem_proc_unregister_locked(p);
  umem_process_finish_locked(p);
  llrb_pid_process_delete(&p->children);
  llrb_tid_thread_delete(&p->threads);
  slab_process_free(p);
  umem_unlock();
}

// Regression test for the paging merge pass: a guard punch fragments the
// kernel tree; reverting it must fold the tables back. Punch inside a
// 2 MiB buddy block: the buddy aligns blocks to their size, so that
// block's PD entry is untouched by anything else (boot-time stack guards
// live in other 2 MiB regions) and the punch is guaranteed to split at
// least the PT — the test can't pass vacuously.
void paging_merge_selftest(void) {
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
void umem_selftest(void) {
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
  asserts(umem_unshare(a, (uint64_t)blk, b->pid) == 0,
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

void device_block_selftest(const struct acpi_rsdp *rsdp,
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
    asserts(umem_dropshare(b, mmio, 0) == 0,
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
void channel_selftest(void) {
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
// threads: zero-thread process creation, one-way VM_MOVE construction,
// retained zombie registry entries, and the tree channel's KEV_CHILD_DEAD.
void process_selftest(void) {
  struct process *parent = process_create(nullptr);
  struct process *child = process_create(parent);
  struct process *grandchild = process_create(child);
  asserts(!child->dead && atomic_load(&child->nthreads) == 0,
          "process selftest: new child not alive and empty");

  // Move a block down into the child: parent view gone, child view RW.
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
  // Descendant death is effective through ancestry. Zombie PID entries
  // remain in the raw registry until exact destruction.
  asserts(!grandchild->dead && process_is_dead(grandchild),
          "process selftest: descendant death was not lazy");
  umem_lock();
  asserts(umem_proc_lookup_locked(grandchild->pid) == nullptr,
          "process selftest: dead pid visible to live lookup");
  asserts(umem_proc_lookup_any_locked(grandchild->pid) == grandchild,
          "process selftest: zombie pid missing from raw lookup");
  umem_unlock();
  volatile struct kring_hdr *h = (volatile struct kring_hdr *)tch;
  const struct kcqe *cq =
      (const struct kcqe *)(tch + KRING_HDR_SIZE +
                            KRING_NSLOTS(0) * sizeof(struct ksqe));
  asserts(h->cq_count == 1 && cq[0].type == KEV_CHILD_DEAD &&
              cq[0].a == child->pid,
          "process selftest: no KEV_CHILD_DEAD");

  // Ownership transfer is construction-only. The live parent instead
  // destroys the dead descendant's block in place.
  asserts(umem_free(parent, (uint64_t)blk) == 0,
          "process selftest: descendant block free failed");
  destroy_test_process(grandchild);
  destroy_test_process(child);
  destroy_test_process(parent);
  printf("process: selftest ok\n");
}
