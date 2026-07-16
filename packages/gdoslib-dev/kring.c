#include "kring.h"

#include <stdatomic.h>
#include <stdint.h>

#include <string.h>

#include <gdos/sys.h>

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

uint64_t kring_cap_open(struct kring *r, uint64_t inherited_base,
                        uint64_t len) {
  if (inherited_base != 0) {
    kring_attach(r, inherited_base);
    return 0;
  }
  return kring_create(r, KSCHEME_CAP, len);
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

static uint64_t data_off(const struct kring *r) {
  return KRING_HDR_SIZE + (uint64_t)r->nslots *
      (sizeof(struct ksqe) + sizeof(struct kcqe));
}

static uint64_t wait_completion(struct kring *r, uint64_t op,
                                struct kcqe *result) {
  for (;;) {
    struct kcqe cqe;
    uint64_t rc = kring_wait_cqe(r, &cqe);
    if (rc != 0) return rc;
    kring_ack(r);
    if (cqe.type == op) {
      if (result != nullptr) *result = cqe;
      return cqe.status;
    }
  }
}

uint64_t kring_cap_subgrant(struct kring *r, const struct cap_token *parent,
                            uint64_t p0, uint64_t p1, uint64_t p2,
                            uint32_t present, struct cap_token *out) {
  uint64_t base = data_off(r);
  struct kcap_subgrant_req *req = (void *)(r->base + base);
  struct cap_token *in = (void *)(r->base + base + 64);
  struct cap_token *result = (void *)(r->base + base + 96);
  *in = *parent;
  *req = (struct kcap_subgrant_req){.token_off = base + 64,
      .token_len = CAP_TOKEN_SIZE, .p0 = p0, .p1 = p1, .p2 = p2,
      .present = present};
  struct ksqe *sqe = kring_get_sqe(r);
  if (sqe == nullptr) return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = KCAP_SUBGRANT, .a = base, .c = base + 96};
  uint64_t rc = kring_submit(r);
  if (rc == 0) rc = wait_completion(r, KCAP_SUBGRANT, nullptr);
  if (rc == 0) *out = *result;
  return rc;
}

uint64_t kring_cap_revoke(struct kring *r, const struct cap_token *token) {
  uint64_t off = data_off(r);
  *(struct cap_token *)(r->base + off) = *token;
  for (;;) {
    struct ksqe *sqe = kring_get_sqe(r);
    if (sqe == nullptr) return SYSERR_AGAIN;
    *sqe = (struct ksqe){.op = KCAP_REVOKE, .a = off,
                         .b = CAP_TOKEN_SIZE};
    uint64_t rc = kring_submit(r);
    if (rc == 0) rc = wait_completion(r, KCAP_REVOKE, nullptr);
    if (rc != SYSERR_AGAIN) return rc;
  }
}

uint64_t kring_irq_claim(struct kring *r, const struct cap_token *token,
                         uint64_t cookie) {
  uint64_t off = data_off(r);
  *(struct cap_token *)(r->base + off) = *token;
  struct ksqe *sqe = kring_get_sqe(r);
  if (sqe == nullptr) return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = KIRQ_CLAIM, .a = off,
      .b = CAP_TOKEN_SIZE, .c = cookie};
  return kring_submit(r);
}

uint64_t kring_irq_release(struct kring *r, uint64_t gsi) {
  return irq_submit(r, KIRQ_RELEASE, gsi, 0);
}

uint64_t kring_irq_ack(struct kring *r, uint64_t gsi, uint64_t seq) {
  return irq_submit(r, KIRQ_ACK, gsi, seq);
}

uint64_t kring_irq_msi(struct kring *r, const struct cap_token *parent,
                       struct cap_token *out, uint64_t *address,
                       uint32_t *data) {
  uint64_t off = data_off(r);
  *(struct cap_token *)(r->base + off) = *parent;
  struct ksqe *sqe = kring_get_sqe(r);
  if (sqe == nullptr) return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = KIRQ_MSI, .a = off, .b = CAP_TOKEN_SIZE,
                       .c = off + CAP_TOKEN_SIZE};
  uint64_t rc = kring_submit(r);
  struct kcqe cqe;
  if (rc == 0) rc = wait_completion(r, KIRQ_MSI, &cqe);
  if (rc == 0) {
    *out = *(struct cap_token *)(r->base + off + CAP_TOKEN_SIZE);
    *address = cqe.a;
    *data = KIRQ_MSI_DATA(cqe.b);
  }
  return rc;
}

uint64_t kring_irq_bind(struct kring *r, const struct cap_token *token,
                        uint64_t cookie, uint32_t *route_id) {
  uint64_t off = data_off(r);
  *(struct cap_token *)(r->base + off) = *token;
  struct ksqe *sqe = kring_get_sqe(r);
  if (sqe == nullptr) return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = KIRQ_BIND, .a = off, .b = CAP_TOKEN_SIZE,
                       .c = cookie};
  uint64_t rc = kring_submit(r);
  struct kcqe cqe;
  if (rc == 0) rc = wait_completion(r, KIRQ_BIND, &cqe);
  if (rc == 0 && route_id != nullptr) *route_id = (uint32_t)cqe.a;
  return rc;
}

uint64_t kring_iommu_attach(struct kring *r, uint64_t domain,
                            const struct cap_token *token,
                            uint64_t fault_cookie) {
  uint64_t off = data_off(r);
  struct kiommu_attach_req *req = (void *)(r->base + off);
  struct cap_token *in = (void *)(r->base + off + 64);
  *in = *token;
  *req = (struct kiommu_attach_req){.domain = domain, .token_off = off + 64,
      .token_len = CAP_TOKEN_SIZE, .fault_cookie = fault_cookie};
  struct ksqe *sqe = kring_get_sqe(r);
  if (sqe == nullptr) return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = KIOMMU_DEVICE_ATTACH, .a = off};
  uint64_t rc = kring_submit(r);
  return rc == 0 ? wait_completion(r, KIOMMU_DEVICE_ATTACH, nullptr) : rc;
}
