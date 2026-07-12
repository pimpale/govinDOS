#include "kring.h"

#include <stdatomic.h>
#include <stdint.h>

#include "sys.h"

// The header words are shared with the kernel: acquire on its mirrors
// (cq_count, sq_head), release on ours (sq_tail, cq_head), exactly the
// discipline the kernel applies from its side (kernel/src/channel.c).

void kring_attach(struct kring *r, uint64_t base) {
  struct kring_hdr *h = (struct kring_hdr *)base;
  r->base = base;
  r->hdr = h;
  r->nslots = h->nslots;
  r->sq = (struct ksqe *)(base + KRING_HDR_SIZE);
  r->cq = (struct kcqe *)(base + KRING_HDR_SIZE +
                          (uint64_t)r->nslots * sizeof(struct ksqe));
  r->sq_tail = atomic_load_explicit(&h->sq_tail, memory_order_relaxed);
  r->cq_head = atomic_load_explicit(&h->cq_head, memory_order_relaxed);
}

uint64_t kring_create(struct kring *r, int64_t scheme, uint64_t len) {
  uint64_t base = sys_vm_alloc(len, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(base)) {
    return base;
  }
  uint64_t rc = sys_vm_share(base, scheme, 0);
  if (rc != 0) {
    sys_vm_free(base);
    return rc;
  }
  kring_attach(r, base);
  return 0;
}

uint64_t kring_destroy(struct kring *r) { return sys_vm_free(r->base); }

struct ksqe *kring_get_sqe(struct kring *r) {
  uint32_t consumed =
      atomic_load_explicit(&r->hdr->sq_head, memory_order_acquire);
  if (r->sq_tail - consumed >= r->nslots) {
    return nullptr;
  }
  return &r->sq[r->sq_tail++ % r->nslots];
}

uint64_t kring_submit(struct kring *r) {
  atomic_store_explicit(&r->hdr->sq_tail, r->sq_tail, memory_order_release);
  // Each doorbell drains at most RING_SQ_BATCH SQEs; re-ring until the
  // kernel's consumption mirror catches up to what we published. Bounded:
  // every doorbell on an honest tail makes progress.
  uint64_t rc;
  do {
    rc = sys_block_doorbell(r->base);
  } while (rc == 0 && atomic_load_explicit(&r->hdr->sq_head,
                                           memory_order_acquire) != r->sq_tail);
  return rc;
}

const struct kcqe *kring_peek_cqe(struct kring *r) {
  uint32_t posted =
      atomic_load_explicit(&r->hdr->cq_count, memory_order_acquire);
  if (posted == r->cq_head) {
    return nullptr;
  }
  return &r->cq[r->cq_head % r->nslots];
}

void kring_cqe_seen(struct kring *r) { r->cq_head++; }

uint64_t kring_wait_cqe(struct kring *r, struct kcqe *cqe) {
  while (1) {
    const struct kcqe *c = kring_peek_cqe(r);
    if (c != nullptr) {
      *cqe = *c;
      kring_cqe_seen(r);
      return 0;
    }
    // Parks while cq_count still equals our consumed head; a post wakes
    // us (the kernel bumps the mirror before the wake). Spurious wakes
    // just loop back into the peek.
    uint64_t rc = sys_block_wait(&r->hdr->cq_count, r->cq_head);
    if (rc != 0) {
      return rc;
    }
  }
}

uint64_t kring_ack(struct kring *r) {
  atomic_store_explicit(&r->hdr->cq_head, r->cq_head, memory_order_release);
  return sys_block_doorbell(r->base);
}

static uint64_t irq_submit(struct kring *r, uint64_t op, uint64_t a,
                           uint64_t b) {
  struct ksqe *sqe = kring_get_sqe(r);
  if (sqe == nullptr)
    return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = op, .a = a, .b = b};
  return kring_submit(r);
}

uint64_t kring_irq_claim(struct kring *r, uint64_t gsi, uint64_t cookie) {
  return irq_submit(r, KIRQ_CLAIM, gsi, cookie);
}

uint64_t kring_irq_release(struct kring *r, uint64_t gsi) {
  return irq_submit(r, KIRQ_RELEASE, gsi, 0);
}

uint64_t kring_irq_ack(struct kring *r, uint64_t gsi, uint64_t seq) {
  return irq_submit(r, KIRQ_ACK, gsi, seq);
}

uint64_t kring_irq_msi(struct kring *r, uint64_t child_pid) {
  return irq_submit(r, KIRQ_MSI, child_pid, 0);
}

uint64_t kring_irq_bind(struct kring *r, uint64_t route_id, uint64_t cookie) {
  return irq_submit(r, KIRQ_BIND, route_id, cookie);
}
