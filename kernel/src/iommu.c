#include "iommu.h"

#include <stdint.h>

#include "channel_internal.h"
#include "capability.h"
#include "cpu_state.h"
#include "debug.h"
#include "iommu_internal.h"
#include "paging.h"
#include "process.h"
#include "spinlock.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "stdlib/string.h"
#include "syscall.h"

#include <gdosabi/kring_iommu.h>

#define IOMMU_MAX_DOMAINS 64
#define IOMMU_MAX_DEVICES 256
#define IOMMU_MAX_MAPPINGS 1024
#define IOMMU_MAX_MAPPING_LEAVES 512

struct iommu_mapping {
  struct iommu_mapping *domain_next;
  struct iommu_mapping *domain_prev;
  struct iommu_domain *domain;
  ublock *block;
  uint32_t permissions;
};
typedef struct iommu_mapping iommu_mapping;

#define LLRB_NAME domid_map
#define LLRB_KEY uint64_t
#define LLRB_VALUE iommu_mapping
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

struct iommu_device {
  struct iommu_device *next;
  struct iommu_device_id id;
  uint64_t encoded;
  uint64_t fault_cookie;
  uint64_t fault_count;
  uint64_t posted_count;
  uint64_t last_iova;
  uint32_t last_reason;
  uint32_t ev_index;
  bool fault_posted;
};

struct iommu_domain {
  struct iommu_domain *next;
  struct ring *ring;
  uint64_t id;
  uint64_t cookie;
  struct iommu_hw_domain hw;
  struct iommu_device *devices;
  struct iommu_mapping *mappings;
};

#define SLAB_NAME iommu_domain
#define SLAB_TYPE struct iommu_domain
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME iommu_device
#define SLAB_TYPE struct iommu_device
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME

static struct iommu_domain *g_domains;
static uint32_t g_ndomains, g_ndevices, g_nmappings;
static uint64_t g_next_domain_id = 1;
static bool g_live;
static struct spinlock g_fault_lock = SPINLOCK_INITIALIZER;
static struct iommu_device *g_fault_devices[1u << 16];

void iommu_init_required(const struct acpi_rsdp *rsdp) {
  spinlock_init(&g_fault_lock);
  if (!vtd_init_required(rsdp))
    fatal("iommu: required VT-d initialization failed\n");
  g_live = true;
  printf("iommu: required mode live, default-deny contexts installed\n");
}

static struct iommu_domain *find_domain(struct ring *ring, uint64_t cookie) {
  for (struct iommu_domain *d = g_domains; d != nullptr; d = d->next)
    if (d->ring == ring && d->cookie == cookie)
      return d;
  return nullptr;
}

static struct iommu_device *find_device(uint64_t encoded,
                                        struct iommu_domain **domain_out) {
  for (struct iommu_domain *d = g_domains; d != nullptr; d = d->next)
    for (struct iommu_device *v = d->devices; v != nullptr; v = v->next)
      if (v->encoded == encoded) {
        if (domain_out != nullptr)
          *domain_out = d;
        return v;
      }
  return nullptr;
}

static bool decode_id(uint64_t encoded, struct iommu_device_id *id) {
  if ((encoded >> 32) != 0)
    return false;
  id->segment = (uint16_t)(encoded >> 16);
  id->bus = (uint8_t)(encoded >> 8);
  id->devfn = (uint8_t)encoded;
  return vtd_covers(*id);
}

static uint64_t domain_create(struct ring *ring, uint64_t cookie) {
  if (!g_live || cookie == 0)
    return SYSERR_INVAL;
  if (find_domain(ring, cookie) != nullptr)
    return SYSERR_EXIST;
  if (g_ndomains == IOMMU_MAX_DOMAINS)
    return SYSERR_NOMEM;
  struct iommu_domain *d = slab_iommu_domain_zalloc(sizeof(*d));
  if (d == nullptr || !vtd_domain_init(&d->hw)) {
    slab_iommu_domain_free(d);
    return SYSERR_NOMEM;
  }
  d->ring = ring;
  d->id = g_next_domain_id++;
  if (d->id == 0)
    fatal("iommu: domain id space exhausted");
  d->cookie = cookie;
  d->next = g_domains;
  g_domains = d;
  g_ndomains++;
  return 0;
}

static uint64_t domain_destroy(struct ring *ring, uint64_t cookie) {
  struct iommu_domain **link = &g_domains;
  while (*link != nullptr) {
    struct iommu_domain *d = *link;
    if (d->ring == ring && d->cookie == cookie) {
      if (d->devices != nullptr || d->mappings != nullptr)
        return SYSERR_EXIST;
      *link = d->next;
      vtd_domain_destroy(&d->hw);
      slab_iommu_domain_free(d);
      g_ndomains--;
      return 0;
    }
    link = &d->next;
  }
  return SYSERR_INVAL;
}

static uint64_t device_attach(struct iommu_domain *d, uint64_t encoded,
                              uint64_t fault_cookie) {
  struct iommu_device_id id;
  if (!decode_id(encoded, &id))
    return SYSERR_INVAL;
  // The caller reached this only after KIOMMU_DEVICE_ATTACH verified the
  // requester capability; this check preserves exclusive attachment.
  if (find_device(encoded, nullptr) != nullptr)
    return SYSERR_EXIST;
  if (g_ndevices == IOMMU_MAX_DEVICES)
    return SYSERR_NOMEM;
  uint32_t ring_devices = 0;
  for (struct iommu_domain *scan = g_domains; scan != nullptr;
       scan = scan->next)
    if (scan->ring == d->ring)
      for (struct iommu_device *dv = scan->devices; dv != nullptr;
           dv = dv->next)
        ring_devices++;
  if (2 * (ring_devices + 1) > d->ring->nslots)
    return SYSERR_NOMEM;
  struct iommu_device *v = slab_iommu_device_zalloc(sizeof(*v));
  if (v == nullptr)
    return SYSERR_NOMEM;
  if (!vtd_attach(&d->hw, id)) {
    slab_iommu_device_free(v);
    return SYSERR_INVAL;
  }
  *v = (struct iommu_device){.next = d->devices,
                             .id = id,
                             .encoded = encoded,
                             .fault_cookie = fault_cookie};
  d->devices = v;
  spinlock_lock(&g_fault_lock);
  g_fault_devices[(uint16_t)encoded] = v;
  spinlock_unlock(&g_fault_lock);
  g_ndevices++;
  printf("iommu: requester %08llX claimed by pid=%llu\n", encoded,
         d->ring->block->owner->pid);
  return 0;
}

static uint64_t device_detach(struct iommu_domain *d, uint64_t encoded) {
  struct iommu_device **link = &d->devices;
  while (*link != nullptr) {
    struct iommu_device *v = *link;
    if (v->encoded == encoded) {
      spinlock_lock(&g_fault_lock);
      g_fault_devices[(uint16_t)v->encoded] = nullptr;
      spinlock_unlock(&g_fault_lock);
      if (!vtd_detach(&d->hw, v->id))
        fatal("iommu: context invalidation failed\n");
      *link = v->next;
      slab_iommu_device_free(v);
      g_ndevices--;
      return 0;
    }
    link = &v->next;
  }
  return SYSERR_INVAL;
}

static void fault_retire_and_post_locked(struct iommu_device *v) {
  struct iommu_domain *d = nullptr;
  (void)find_device(v->encoded, &d);
  if (d == nullptr)
    return;
  if (v->fault_posted && cqe_consumed(d->ring, v->ev_index))
    v->fault_posted = false;
  if (v->fault_posted || v->posted_count == v->fault_count)
    return;
  uint32_t index;
  if (channel_post_data(d->ring, KEV_IOMMU_FAULT, v->fault_cookie,
                        v->last_iova, v->last_reason, &index)) {
    v->fault_posted = true;
    v->ev_index = index;
    v->posted_count = v->fault_count;
  }
}

void iommu_report_fault(uint64_t requester, uint64_t iova, uint32_t reason) {
  if (requester >> 16)
    return;
  spinlock_lock(&g_fault_lock);
  struct iommu_device *v = g_fault_devices[(uint16_t)requester];
  if (v != nullptr) {
    v->fault_count++;
    v->last_iova = iova;
    v->last_reason = reason;
    fault_retire_and_post_locked(v);
  }
  spinlock_unlock(&g_fault_lock);
}

void iommu_replay(struct ring *ring) {
  spinlock_lock(&g_fault_lock);
  for (struct iommu_domain *d = g_domains; d != nullptr; d = d->next)
    if (d->ring == ring)
      for (struct iommu_device *v = d->devices; v != nullptr; v = v->next)
        fault_retire_and_post_locked(v);
  spinlock_unlock(&g_fault_lock);
}

static struct iommu_mapping *find_mapping(struct iommu_domain *d,
                                           uint64_t base) {
  for (struct iommu_mapping *m = d->mappings; m != nullptr;
       m = m->domain_next)
    if (m->block->base == base)
      return m;
  return nullptr;
}

static uint64_t map_block(struct process *p, struct iommu_domain *d,
                          uint64_t base, uint32_t permissions) {
  if (permissions == 0 ||
      (permissions & ~(IOMMU_PERM_DEVICE_READ | IOMMU_PERM_DEVICE_WRITE)))
    return SYSERR_INVAL;
  if (find_mapping(d, base) != nullptr)
    return SYSERR_EXIST;
  if (g_nmappings == IOMMU_MAX_MAPPINGS)
    return SYSERR_NOMEM;
  umem_proc_lock(p);
  ublock *b = umem_view_locked(p, base, 1);
  if (b == nullptr || b->base != base || b->backing != UBLOCK_RAM ||
      b->ring != nullptr) {
    umem_proc_unlock(p);
    return SYSERR_INVAL;
  }
  uint64_t pages = 1ull << b->order;
  if (vtd_mapping_leaves(base, pages) > IOMMU_MAX_MAPPING_LEAVES) {
    umem_proc_unlock(p);
    return SYSERR_NOMEM;
  }
  umem_proc_unlock(p);
  llrb_domid_map_node *map_node =
      slab_llrb_domid_map_node_malloc(sizeof(*map_node));
  if (map_node == nullptr)
    return SYSERR_NOMEM;
  llrb_domid_map *new_index = nullptr;
  if (b->dma_maps == nullptr && !llrb_domid_map_new(&new_index)) {
    slab_llrb_domid_map_node_free(map_node);
    return SYSERR_NOMEM;
  }
  if (!vtd_map(&d->hw, base, base, pages, permissions)) {
    llrb_domid_map_delete(&new_index);
    slab_llrb_domid_map_node_free(map_node);
    return SYSERR_NOMEM;
  }

  bool new_tree = b->dma_maps == nullptr;
  if (new_tree) {
    b->dma_maps = new_index;
    new_index = nullptr;
  }
  iommu_mapping value = {.domain_next = d->mappings,
                         .domain = d,
                         .block = b,
                         .permissions = permissions};
  if (!llrb_domid_map_insert_node(b->dma_maps, &d->id, &value, map_node)) {
    if (new_tree) {
      llrb_domid_map_delete(&b->dma_maps);
      b->dma_maps = nullptr;
    } else {
      llrb_domid_map_delete(&new_index);
    }
    (void)vtd_unmap(&d->hw, base, pages);
    slab_llrb_domid_map_node_free(map_node);
    return SYSERR_NOMEM;
  }
  llrb_domid_map_delete(&new_index);
  iommu_mapping *m;
  asserts(llrb_domid_map_get_ref(b->dma_maps, &d->id, &m),
          "iommu: inserted mapping missing");
  if (d->mappings != nullptr)
    d->mappings->domain_prev = m;
  d->mappings = m;
  g_nmappings++;
  return 0;
}

static void unlink_mapping(struct iommu_mapping *m) {
  struct iommu_domain *d = m->domain;
  if (m->domain_prev != nullptr)
    m->domain_prev->domain_next = m->domain_next;
  else
    d->mappings = m->domain_next;
  if (m->domain_next != nullptr)
    m->domain_next->domain_prev = m->domain_prev;
  ublock *b = m->block;
  uint64_t id = d->id;
  iommu_mapping removed;
  asserts(llrb_domid_map_remove(b->dma_maps, &id, &removed),
          "iommu: block mapping missing");
  if (llrb_domid_map_len(b->dma_maps) == 0) {
    llrb_domid_map_delete(&b->dma_maps);
    b->dma_maps = nullptr;
  }
}

static void unmap_mapping(struct iommu_mapping *m) {
  uint64_t base = m->block->base;
  uint64_t pages = 1ull << m->block->order;
  if (!vtd_unmap(&m->domain->hw, base, pages))
    fatal("iommu: IOTLB invalidation failed\n");
  unlink_mapping(m);
  g_nmappings--;
}

static uint64_t unmap_block(struct iommu_domain *d, uint64_t base) {
  for (struct iommu_mapping *m = d->mappings; m != nullptr;
       m = m->domain_next) {
    if (m->block->base == base) {
      unmap_mapping(m);
      return 0;
    }
  }
  return SYSERR_INVAL;
}

uint64_t iommu_exec(struct thread *curr, struct ring *ring,
                    struct ksqe *sqe) {
  struct iommu_domain *d;
  switch (sqe->op) {
  case KIOMMU_DOMAIN_CREATE:
    return sqe->b == 0 && sqe->c == 0 ? domain_create(ring, sqe->a)
                                      : SYSERR_INVAL;
  case KIOMMU_DOMAIN_DESTROY:
    return domain_destroy(ring, sqe->a);
  case KIOMMU_DEVICE_ATTACH:
    if (sqe->a > ring->block->bytes ||
        sizeof(struct kiommu_attach_req) > ring->block->bytes - sqe->a)
      return SYSERR_INVAL;
    struct kiommu_attach_req req;
    memcpy(&req, (const void *)(ring->block->base + sqe->a), sizeof(req));
    d = find_domain(ring, req.domain);
    if (d == nullptr) return SYSERR_INVAL;
    grant *g;
    uint64_t rc = cap_verify_ring_locked(ring, req.token_off, req.token_len,
                                         KCAP_GRANT_IOMMU_DEV, &g);
    return rc == 0 ? device_attach(d, cap_iommu_requester(g), req.fault_cookie)
                   : rc;
  case KIOMMU_DEVICE_DETACH:
    d = find_domain(ring, sqe->a);
    return d != nullptr ? device_detach(d, sqe->b) : SYSERR_INVAL;
  case KIOMMU_MAP_BLOCK:
    d = find_domain(ring, sqe->a);
    return d != nullptr ? map_block(curr->proc, d, sqe->b, (uint32_t)sqe->c)
                        : SYSERR_INVAL;
  case KIOMMU_UNMAP_BLOCK:
    d = find_domain(ring, sqe->a);
    return d != nullptr ? unmap_block(d, sqe->b) : SYSERR_INVAL;
  default:
    return SYSERR_NOSYS;
  }
}

bool iommu_endpoint_destroyable(struct ring *ring) {
  (void)ring;
  return true;
}

void iommu_endpoint_destroy(struct ring *ring) {
  struct iommu_domain **link = &g_domains;
  while (*link != nullptr) {
    struct iommu_domain *d = *link;
    if (d->ring != ring) {
      link = &d->next;
      continue;
    }
    while (d->devices != nullptr)
      (void)device_detach(d, d->devices->encoded);
    while (d->mappings != nullptr)
      unmap_mapping(d->mappings);
    *link = d->next;
    vtd_domain_destroy(&d->hw);
    slab_iommu_domain_free(d);
    g_ndomains--;
  }
}

uint64_t iommu_enum_maps(struct process *caller, uint64_t base, uint64_t buf,
                         uint64_t cap, uint64_t after) {
  if (cap == 0 || cap > VM_ENUM_BATCH)
    return SYSERR_INVAL;
  uint64_t values[VM_ENUM_BATCH], count = 0;
  umem_lock();
  ublock *b = umem_resource_block_locked(caller, base);
  if (b == nullptr) {
    umem_unlock();
    return SYSERR_PERM;
  }
  if (b->dma_maps != nullptr) {
    llrb_domid_map_iter iter;
    llrb_domid_map_iter_lower_bound(b->dma_maps, &after, &iter);
    uint64_t key;
    while (count < cap &&
           llrb_domid_map_iter_next(&iter, &key, nullptr))
      if (key > after)
        values[count++] = key;
  }
  uint64_t rc = umem_enum_copyout_locked(caller, buf, cap, values, count);
  umem_unlock();
  return rc;
}

uint64_t iommu_revoke_map(struct process *caller, uint64_t base,
                          uint64_t domain_id) {
  umem_lock();
  ublock *b = umem_resource_block_locked(caller, base);
  if (b == nullptr) {
    umem_unlock();
    return SYSERR_PERM;
  }
  iommu_mapping *m = nullptr;
  if (b->dma_maps != nullptr)
    (void)llrb_domid_map_get_ref(b->dma_maps, &domain_id, &m);
  if (m != nullptr) {
    unmap_mapping(m);
    umem_unlock();
    return 0;
  }
  umem_unlock();
  return SYSERR_INVAL;
}

// The block-side tree owns each mapping inline; its node slab lives beside
// the tree instantiation so the pair remains one implementation unit.
#define LLRB_NAME domid_map
#define LLRB_KEY uint64_t
#define LLRB_VALUE iommu_mapping
#define LLRB_COMPARE(a, b) (((*a) > (*b)) - ((*a) < (*b)))
#define LLRB_NODE_MALLOC(size) slab_llrb_domid_map_node_malloc(size)
#define LLRB_NODE_FREE(ptr) slab_llrb_domid_map_node_free(ptr)
#include <llrb/llrb_impl.h>

#define SLAB_NAME llrb_domid_map_node
#define SLAB_TYPE llrb_domid_map_node
#define SLAB_PAGE_SIZE PAGE_SIZE
#define SLAB_CACHELINE_SIZE 64
#define SLAB_WHICH_CPU() cpu_state_whoami()
#include <slab/slab_impl.h>
#undef SLAB_WHICH_CPU
#undef SLAB_CACHELINE_SIZE
#undef SLAB_PAGE_SIZE
#undef SLAB_TYPE
#undef SLAB_NAME
