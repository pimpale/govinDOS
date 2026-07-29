#include "capability.h"

#include <stddef.h>

#include "channel_internal.h"
#include "cpu_state.h"
#include "debug.h"
#include "irq_scheme.h"
#include "paging.h"
#include "sha256.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "thread.h"
#include "uaccess.h"
#include "umem.h"

#include <gdosabi/syscall.h>

struct grant {
  uint64_t id;
  uint8_t type;
  uint8_t depth;
  bool wildcard;
  bool dead;
  union {
    struct { uint64_t base, len; uint32_t flags; } devmem;
    struct { struct irq_route *route; uint32_t id; } irq;
    struct { uint32_t requester; } iommu;
  };
  grant *parent;
  grant *first_child;
  grant *next_sibling;
  grant *prev_sibling;
};

#define SLAB_NAME grant
#define SLAB_TYPE grant
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME

static llrb_grant_id *g_grants;
static uint64_t g_next_grant_id = 1;
static uint8_t g_boot_key[32];
static const uint8_t g_mac_domain[] = "govindos-cap-v1";

static bool random64(uint64_t *out) {
  uint32_t a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(1), "c"(0));
  if (!(c & (1u << 30))) return false;
  for (uint32_t i = 0; i < 16; i++) {
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    if (ok) return true;
  }
  return false;
}

static void token_mac(const struct cap_token *token, uint8_t out[32]) {
  uint8_t input[sizeof(g_mac_domain) - 1 + 16];
  memcpy(input, g_mac_domain, sizeof(g_mac_domain) - 1);
  memcpy(input + sizeof(g_mac_domain) - 1, token, 16);
  hmac_sha256(g_boot_key, sizeof(g_boot_key), input, sizeof(input), out);
}

static void token_write(const grant *g, struct cap_token *out) {
  *out = (struct cap_token){.version = CAP_TOKEN_VERSION,
                             .grant_id_le = g->id};
  uint8_t mac[32];
  token_mac(out, mac);
  memcpy(out->mac, mac, sizeof(out->mac));
}

static bool constant_equal(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t different = 0;
  for (size_t i = 0; i < len; i++) different |= a[i] ^ b[i];
  return different == 0;
}

static uint64_t token_resolve(const struct cap_token *token, bool need_live,
                              uint8_t type, grant **out) {
  if (token->version != CAP_TOKEN_VERSION || token->nres != 0)
    return SYSERR_INVAL;
  for (size_t i = 0; i < sizeof(token->reserved); i++)
    if (token->reserved[i] != 0) return SYSERR_INVAL;
  uint8_t mac[32];
  token_mac(token, mac);
  if (!constant_equal(token->mac, mac, sizeof(token->mac)))
    return SYSERR_PERM;
  grant *g;
  uint64_t id = token->grant_id_le;
  if (!llrb_grant_id_get(g_grants, &id, &g)) return SYSERR_DEAD;
  if (type != 0 && g->type != type) return SYSERR_PERM;
  if (need_live) {
    uint32_t walked = 0;
    for (grant *at = g; at != nullptr; at = at->parent) {
      if (at->dead) return SYSERR_DEAD;
      if (++walked > KCAP_MAX_DEPTH + 1) return SYSERR_INVAL;
    }
  }
  *out = g;
  return 0;
}

static bool ring_range(const struct ring *ring, uint64_t off, uint64_t len) {
  return off <= ring->block->bytes && len <= ring->block->bytes - off;
}

static uint64_t resolve_ring(struct ring *ring, uint64_t off, uint64_t len,
                             bool need_live, uint8_t type, grant **out) {
  if (len != CAP_TOKEN_SIZE || !ring_range(ring, off, len))
    return SYSERR_INVAL;
  struct cap_token token;
  memcpy(&token, (const void *)(ring->block->base + off), sizeof(token));
  return token_resolve(&token, need_live, type, out);
}

uint64_t cap_verify_ring_locked(struct ring *ring, uint64_t off, uint64_t len,
                                uint8_t type, grant **out) {
  return resolve_ring(ring, off, len, true, type, out);
}

static grant *grant_new(grant *parent, uint8_t type, bool wildcard) {
  if (parent != nullptr && parent->depth >= KCAP_MAX_DEPTH) return nullptr;
  grant *g = slab_grant_zalloc(sizeof(*g));
  if (g == nullptr) return nullptr;
  g->id = g_next_grant_id++;
  if (g->id == 0) fatal("capability: grant id exhausted");
  g->type = type;
  g->depth = parent == nullptr ? 0 : parent->depth + 1;
  g->wildcard = wildcard;
  g->parent = parent;
  if (parent != nullptr) {
    g->next_sibling = parent->first_child;
    if (g->next_sibling != nullptr) g->next_sibling->prev_sibling = g;
    parent->first_child = g;
  }
  if (!llrb_grant_id_insert(g_grants, &g->id, &g))
    fatal("capability: grant index insertion failed");
  return g;
}

static void grant_delete_leaf(grant *g) {
  asserts(g->first_child == nullptr, "capability: deleting non-leaf");
  if (g->parent != nullptr) {
    if (g->prev_sibling != nullptr) g->prev_sibling->next_sibling = g->next_sibling;
    else g->parent->first_child = g->next_sibling;
    if (g->next_sibling != nullptr) g->next_sibling->prev_sibling = g->prev_sibling;
  }
  grant *removed = nullptr;
  asserts(llrb_grant_id_remove(g_grants, &g->id, &removed) && removed == g,
          "capability: grant index removal failed");
  slab_grant_free(g);
}

static uint64_t revoke_step(grant *target) {
  target->dead = true;
  grant *leaf = target;
  while (leaf->first_child != nullptr) leaf = leaf->first_child;
  bool final = leaf == target;
  grant_delete_leaf(leaf);
  return final ? 0 : SYSERR_AGAIN;
}

static bool add_overflows(uint64_t base, uint64_t len, uint64_t *end) {
  return len == 0 || __builtin_add_overflow(base, len, end);
}

static uint64_t subgrant(const struct kcap_subgrant_req *req, grant *parent,
                         struct cap_token *token) {
  if (req->reserved != 0 || req->reserved2 != 0 ||
      (req->present & ~7u) != 0)
    return SYSERR_INVAL;
  if (parent->depth >= KCAP_MAX_DEPTH) return SYSERR_NOMEM;
  grant *child = grant_new(parent, parent->type, false);
  if (child == nullptr) return SYSERR_NOMEM;
  uint64_t rc = 0;
  if (parent->type == KCAP_GRANT_DEVMEM) {
    if (parent->wildcard && req->present != 7u) rc = SYSERR_INVAL;
    uint64_t base = req->present & KCAP_PARAM_P0 ? req->p0 : parent->devmem.base;
    uint64_t len = req->present & KCAP_PARAM_P1 ? req->p1 : parent->devmem.len;
    uint32_t flags = req->present & KCAP_PARAM_P2 ? (uint32_t)req->p2
                                                  : parent->devmem.flags;
    uint64_t end, parent_end;
    if (((req->present & KCAP_PARAM_P2) && req->p2 >> 32) ||
        add_overflows(base, len, &end) ||
        (!parent->wildcard &&
         (add_overflows(parent->devmem.base, parent->devmem.len, &parent_end) ||
          base < parent->devmem.base || end > parent_end)) ||
        (!parent->wildcard && (flags & ~parent->devmem.flags))) rc = SYSERR_PERM;
    child->devmem = (typeof(child->devmem)){base, len, flags};
  } else if (parent->type == KCAP_GRANT_IRQ_ROUTE) {
    if (parent->wildcard) {
      if (!(req->present & KCAP_PARAM_P0)) rc = SYSERR_INVAL;
      else child->irq.route = irq_route_for_gsi_locked(req->p0);
      if (child->irq.route == nullptr) rc = SYSERR_INVAL;
      else child->irq.id = irq_route_id_locked(child->irq.route);
    } else {
      if ((req->present & KCAP_PARAM_P0) &&
          req->p0 != parent->irq.id) rc = SYSERR_PERM;
      child->irq = parent->irq;
    }
    if (req->present & (KCAP_PARAM_P1 | KCAP_PARAM_P2)) rc = SYSERR_INVAL;
  } else if (parent->type == KCAP_GRANT_IOMMU_DEV) {
    uint64_t requester = req->present & KCAP_PARAM_P0
                             ? req->p0 : parent->iommu.requester;
    if (requester >> 32 || (parent->wildcard && !(req->present & KCAP_PARAM_P0)) ||
        (!parent->wildcard && requester != parent->iommu.requester) ||
        (req->present & (KCAP_PARAM_P1 | KCAP_PARAM_P2))) rc = SYSERR_PERM;
    child->iommu.requester = (uint32_t)requester;
  } else rc = SYSERR_INVAL;
  if (rc != 0) {
    grant_delete_leaf(child);
    return rc;
  }
  token_write(child, token);
  return 0;
}

uint64_t cap_exec(struct thread *curr, struct ring *ring, struct ksqe *sqe) {
  if (sqe->op == KCAP_SUBGRANT) {
    if (sqe->b != 0 || !ring_range(ring, sqe->a, sizeof(struct kcap_subgrant_req)) ||
        !ring_range(ring, sqe->c, sizeof(struct cap_token))) return SYSERR_INVAL;
    struct kcap_subgrant_req req;
    memcpy(&req, (const void *)(ring->block->base + sqe->a), sizeof(req));
    grant *parent;
    uint64_t rc = cap_verify_ring_locked(ring, req.token_off, req.token_len, 0,
                                         &parent);
    if (rc != 0) return rc;
    struct cap_token out;
    rc = subgrant(&req, parent, &out);
    if (rc == 0) {
      memcpy((void *)(ring->block->base + sqe->c), &out, sizeof(out));
      sqe->a = out.grant_id_le;
    }
    return rc;
  }
  if (sqe->op == KCAP_REVOKE) {
    if (sqe->b != CAP_TOKEN_SIZE || !ring_range(ring, sqe->a, sqe->b))
      return SYSERR_INVAL;
    struct cap_token token;
    memcpy(&token, (const void *)(ring->block->base + sqe->a), sizeof(token));
    grant *target;
    uint64_t rc = token_resolve(&token, false, 0, &target);
    return rc == 0 ? revoke_step(target) : rc;
  }
  if (sqe->op == KCAP_QUERY) {
    if (!ring_range(ring, sqe->a, sizeof(struct kcap_query_req)) ||
        !ring_range(ring, sqe->c, sizeof(struct kcap_query_result)))
      return SYSERR_INVAL;
    struct kcap_query_req req;
    memcpy(&req, (const void *)(ring->block->base + sqe->a), sizeof(req));
    grant *g;
    uint64_t rc = resolve_ring(ring, req.token_off, req.token_len, false, 0, &g);
    if (rc != 0) return rc;
    bool live = true;
    for (grant *at = g; at != nullptr; at = at->parent)
      if (at->dead) live = false;
    struct kcap_query_result q = {.grant_id = g->id, .type = g->type,
        .depth = g->depth, .live = live, .wildcard = g->wildcard};
    if (g->type == KCAP_GRANT_DEVMEM) {
      q.p0 = g->devmem.base; q.p1 = g->devmem.len; q.p2 = g->devmem.flags;
    } else if (g->type == KCAP_GRANT_IRQ_ROUTE) {
      q.p0 = g->wildcard ? UINT64_MAX : g->irq.id;
    } else q.p0 = g->wildcard ? UINT64_MAX : g->iommu.requester;
    memcpy((void *)(ring->block->base + sqe->c), &q, sizeof(q));
    return 0;
  }
  return SYSERR_NOSYS;
}

void capability_init(void) {
  static const uint8_t abc_digest[32] = {
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,
      0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,
      0xf2,0x00,0x15,0xad};
  struct sha256_ctx sha;
  uint8_t digest[32];
  sha256_init(&sha); sha256_update(&sha, "abc", 3); sha256_final(&sha, digest);
  asserts(constant_equal(digest, abc_digest, sizeof(digest)),
          "capability: SHA-256 selftest failed");
  asserts(llrb_grant_id_new(&g_grants), "capability: index alloc failed");
  for (uint32_t i = 0; i < sizeof(g_boot_key) / 8; i++) {
    uint64_t random;
    asserts(random64(&random), "capability: CPU random source unavailable");
    memcpy(g_boot_key + 8 * i, &random, sizeof(random));
  }
}

void capability_selftest(struct process *init,
                         const struct cap_token *devmem_root) {
  umem_lock();
  grant *root;
  asserts(token_resolve(devmem_root, true, KCAP_GRANT_DEVMEM, &root) == 0,
          "capability selftest: root token rejected");
  struct cap_token bad = *devmem_root;
  bad.mac[0] ^= 1;
  grant *ignored;
  asserts(token_resolve(&bad, true, KCAP_GRANT_DEVMEM, &ignored) == SYSERR_PERM,
          "capability selftest: forged token accepted");

  struct kcap_subgrant_req req = {.p0 = 0, .p1 = PAGE_SIZE,
      .p2 = VM_DEVICE_READ,
      .present = KCAP_PARAM_P0 | KCAP_PARAM_P1 | KCAP_PARAM_P2};
  struct cap_token token;
  asserts(subgrant(&req, root, &token) == 0,
          "capability selftest: zero-base subgrant failed");
  struct cap_token anchor_token = token;
  grant *anchor;
  asserts(token_resolve(&token, true, KCAP_GRANT_DEVMEM, &anchor) == 0,
          "capability selftest: child token rejected");
  grant *parent = anchor;
  req = (struct kcap_subgrant_req){0};
  for (uint32_t depth = 2; depth <= KCAP_MAX_DEPTH; depth++) {
    asserts(subgrant(&req, parent, &token) == 0,
            "capability selftest: bounded chain creation failed");
    asserts(token_resolve(&token, true, KCAP_GRANT_DEVMEM, &parent) == 0,
            "capability selftest: chain token rejected");
  }
  asserts(subgrant(&req, parent, &token) == SYSERR_NOMEM,
          "capability selftest: depth-65 grant accepted");
  uint64_t rc = revoke_step(anchor);
  asserts(rc == SYSERR_AGAIN &&
              token_resolve(&anchor_token, true, KCAP_GRANT_DEVMEM, &ignored) ==
                  SYSERR_DEAD,
          "capability selftest: revoke was not immediately effective");
  do { rc = revoke_step(anchor); } while (rc == SYSERR_AGAIN);
  asserts(rc == 0, "capability selftest: retry cleanup failed");
  umem_unlock();
  printf("capability: token/depth/revoke selftest ok\n");
}

void capability_bootstrap(struct process *init, struct cap_token *devmem,
                          struct cap_token *irq, struct cap_token *iommu) {
  umem_lock();
  (void)init;
  grant *d = grant_new(nullptr, KCAP_GRANT_DEVMEM, true);
  grant *q = grant_new(nullptr, KCAP_GRANT_IRQ_ROUTE, true);
  grant *m = grant_new(nullptr, KCAP_GRANT_IOMMU_DEV, true);
  asserts(d && q && m, "capability: root grant allocation failed");
  token_write(d, devmem); token_write(q, irq); token_write(m, iommu);
  umem_unlock();
}

struct irq_route *cap_irq_route(const grant *g) {
  if (g->irq.route == nullptr || irq_route_id_locked(g->irq.route) != g->irq.id)
    return nullptr;
  return g->irq.route;
}
uint32_t cap_iommu_requester(const grant *g) { return g->iommu.requester; }

uint64_t cap_create_irq_route_locked(grant *parent, struct irq_route *route,
                                     struct cap_token *out) {
  if (parent->type != KCAP_GRANT_IRQ_ROUTE || parent->dead ||
      !parent->wildcard) return SYSERR_PERM;
  grant *g = grant_new(parent, KCAP_GRANT_IRQ_ROUTE, false);
  if (g == nullptr) return SYSERR_NOMEM;
  g->irq.route = route;
  g->irq.id = irq_route_id_locked(route);
  token_write(g, out);
  return 0;
}

uint64_t cap_map_device(struct process *p, uint64_t token_ptr,
                        uint64_t token_len, uint32_t flags) {
  if (token_len != CAP_TOKEN_SIZE) return SYSERR_INVAL;
  if (!user_range_ok(p, token_ptr, token_len, false)) return SYSERR_FAULT;
  struct cap_token token;
  memcpy(&token, (const void *)token_ptr, sizeof(token));
  umem_lock();
  grant *g;
  uint64_t rc = token_resolve(&token, true, KCAP_GRANT_DEVMEM, &g);
  if (rc == 0 && (g->wildcard || (flags & ~g->devmem.flags))) rc = SYSERR_PERM;
  if (rc == 0)
    rc = umem_map_device_locked(p, g->devmem.base, g->devmem.len, flags);
  umem_unlock();
  return rc;
}
