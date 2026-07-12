#include "channel_internal.h"

#include <stdint.h>

#include "debug.h"
#include "ioapic.h"
#include "irq_scheme.h"
#include "lapic.h"
#include "spinlock.h"
#include "stdlib/stdio.h"
#include "syscall.h"

#define IRQ_ROUTE_COUNT (VECTOR_DEVICE_END - VECTOR_DEVICE_BASE + 1)

struct irq_route {
  struct spinlock lock;
  struct ring *ring;
  struct irq_route *next_claim;
  uint64_t cookie;
  uint64_t raised;
  uint64_t acked;
  uint32_t ev_index;
  uint32_t gsi;
  enum irq_trigger_mode mode;
  struct process *target;
  uint16_t generation;
  bool present;
  bool allocated;
  bool posted;
  bool spurious_logged;
};

// Routes are keyed by device vector: slot i serves VECTOR_DEVICE_BASE+i.
// The pin policy assigns a claimed GSI the vector base+gsi, so for pin
// IRQs slot index == gsi and the SQE vocabulary's "gsi" is really this
// route index. Phase-2 MSI allocates a slot with no IOAPIC pin behind it
// (present == false) and hands its index back as the pseudo-gsi for
// KIRQ_ACK/KIRQ_RELEASE — same table, same ops, no new namespace.
static struct irq_route g_routes[IRQ_ROUTE_COUNT];
static uint8_t g_bsp_apic_id;

static struct irq_route *route_for_gsi(uint64_t gsi) {
  if (gsi >= IRQ_ROUTE_COUNT || !g_routes[gsi].present)
    return nullptr;
  return &g_routes[gsi];
}

static uint32_t route_id(const struct irq_route *route) {
  return ((uint32_t)route->generation << 16) | route->gsi;
}

static struct irq_route *route_for_id(uint64_t id) {
  if (id < IRQ_ROUTE_COUNT && g_routes[id].present)
    return &g_routes[id];
  uint32_t idx = (uint32_t)id & 0xffff;
  uint16_t generation = (uint16_t)((uint32_t)id >> 16);
  if (idx >= IRQ_ROUTE_COUNT || generation == 0)
    return nullptr;
  struct irq_route *route = &g_routes[idx];
  return route->allocated && route->generation == generation ? route : nullptr;
}

static void retire_locked(struct irq_route *route) {
  if (route->posted && route->ring != nullptr &&
      cqe_consumed(route->ring, route->ev_index))
    route->posted = false;
}

static void post_pending_locked(struct irq_route *route) {
  retire_locked(route);
  if (route->ring == nullptr || route->posted ||
      route->raised == route->acked)
    return;
  uint32_t index;
  if (channel_post_data(route->ring, KEV_IRQ, route->cookie, route->raised, 0,
                        &index)) {
    route->posted = true;
    route->ev_index = index;
  }
}

void irq_scheme_init(const struct acpi_madt *madt) {
  x86_ioapic_init(madt);
  g_bsp_apic_id = x86_lapic_id();
  for (uint32_t gsi = 0; gsi < IRQ_ROUTE_COUNT; gsi++) {
    struct irq_route *r = &g_routes[gsi];
    spinlock_init(&r->lock);
    r->gsi = gsi;
    r->present = x86_ioapic_gsi_info(gsi, &r->mode);
  }
}

void irq_deliver(uint8_t vector) {
  uint32_t gsi = (uint32_t)vector - VECTOR_DEVICE_BASE;
  struct irq_route *route = &g_routes[gsi];
  spinlock_lock(&route->lock);
  if (route->ring == nullptr) {
    if (route->present)
      x86_ioapic_mask(route->gsi);
    bool log = !route->spurious_logged;
    route->spurious_logged = true;
    spinlock_unlock(&route->lock);
    x86_lapic_eoi();
    if (log) // serial I/O stays outside the route lock
      printf("irq: masked unclaimed vector=%u gsi=%u\n", vector, gsi);
    return;
  }
  if (route->mode == IRQ_TRIGGER_LEVEL)
    x86_ioapic_mask(route->gsi);
  route->raised++;
  post_pending_locked(route);
  spinlock_unlock(&route->lock);
  x86_lapic_eoi();
}

static uint64_t claim(struct ring *ring, uint64_t gsi, uint64_t cookie) {
  struct irq_route *route = route_for_gsi(gsi);
  if (route == nullptr)
    return SYSERR_INVAL;
  if (2 * (ring->nclaims + 1) > ring->nslots)
    return SYSERR_NOMEM;

  spinlock_lock(&route->lock);
  if (route->ring != nullptr) {
    spinlock_unlock(&route->lock);
    return SYSERR_EXIST;
  }
  // Install a masked RTE before publishing its ring, then unmask only once
  // every field the handler consumes is live under the route lock.
  if (!x86_ioapic_program((uint32_t)gsi,
                          (uint8_t)(VECTOR_DEVICE_BASE + gsi),
                          g_bsp_apic_id)) {
    spinlock_unlock(&route->lock);
    return SYSERR_INVAL;
  }
  route->cookie = cookie;
  route->raised = 0;
  route->acked = 0;
  route->posted = false;
  route->spurious_logged = false;
  route->ring = ring;
  route->next_claim = ring->irq_claims;
  ring->irq_claims = route;
  ring->nclaims++;
  x86_ioapic_unmask((uint32_t)gsi);
  spinlock_unlock(&route->lock);
  return 0;
}

static uint64_t release(struct ring *ring, uint64_t gsi) {
  struct irq_route *route = route_for_id(gsi);
  if (route == nullptr)
    return SYSERR_INVAL;
  spinlock_lock(&route->lock);
  if (route->ring != ring) {
    spinlock_unlock(&route->lock);
    return SYSERR_INVAL;
  }
  if (route->present)
    x86_ioapic_mask(route->gsi);
  route->ring = nullptr;
  route->posted = false;
  spinlock_unlock(&route->lock);

  struct irq_route **link = &ring->irq_claims;
  while (*link != nullptr && *link != route)
    link = &(*link)->next_claim;
  asserts(*link == route, "irq: claim missing from ring");
  *link = route->next_claim;
  route->next_claim = nullptr;
  ring->nclaims--;
  if (!route->present) {
    route->allocated = false;
    route->target = nullptr;
  }
  return 0;
}

static uint64_t ack(struct ring *ring, uint64_t gsi, uint64_t seq) {
  struct irq_route *route = route_for_id(gsi);
  if (route == nullptr)
    return SYSERR_INVAL;
  spinlock_lock(&route->lock);
  if (route->ring != ring) {
    spinlock_unlock(&route->lock);
    return SYSERR_INVAL;
  }
  route->acked = seq;
  if (route->mode == IRQ_TRIGGER_LEVEL)
    x86_ioapic_unmask(route->gsi);
  post_pending_locked(route);
  spinlock_unlock(&route->lock);
  return 0;
}

static uint64_t allocate_msi(struct thread *curr, struct ksqe *sqe) {
  struct process *target = umem_proc_lookup_locked(sqe->a);
  // TODO(capability): replace direct-child provenance with CAP_IRQ.
  if (target == nullptr || target->parent != curr->proc)
    return SYSERR_INVAL;
  for (uint32_t i = IRQ_ROUTE_COUNT; i-- > 0;) {
    struct irq_route *route = &g_routes[i];
    if (route->present || route->allocated)
      continue;
    spinlock_lock(&route->lock);
    if (route->allocated) {
      spinlock_unlock(&route->lock);
      continue;
    }
    route->generation++;
    if (route->generation == 0)
      route->generation++;
    route->allocated = true;
    route->target = target;
    route->mode = IRQ_TRIGGER_EDGE;
    route->raised = 0;
    route->acked = 0;
    route->posted = false;
    route->spurious_logged = false;
    uint32_t id = route_id(route);
    uint32_t data = VECTOR_DEVICE_BASE + i;
    sqe->a = 0xFEE00000ull | ((uint64_t)g_bsp_apic_id << 12);
    sqe->b = KIRQ_MSI_PACK(id, data);
    spinlock_unlock(&route->lock);
    return 0;
  }
  return SYSERR_NOMEM;
}

static uint64_t bind_msi(struct process *owner, struct ring *ring,
                         uint64_t id, uint64_t cookie) {
  struct irq_route *route = route_for_id(id);
  if (route == nullptr || route->present ||
      2 * (ring->nclaims + 1) > ring->nslots)
    return SYSERR_INVAL;
  spinlock_lock(&route->lock);
  // TODO(capability): target provenance becomes route-cap possession.
  if (!route->allocated || route->target != owner || route->ring != nullptr) {
    spinlock_unlock(&route->lock);
    return SYSERR_PERM;
  }
  route->cookie = cookie;
  route->ring = ring;
  route->next_claim = ring->irq_claims;
  ring->irq_claims = route;
  ring->nclaims++;
  spinlock_unlock(&route->lock);
  return 0;
}

uint64_t irq_exec(struct thread *curr, struct ring *ring,
                  struct ksqe *sqe) {
  switch (sqe->op) {
  case KIRQ_CLAIM:
    return claim(ring, sqe->a, sqe->b);
  case KIRQ_RELEASE:
    return release(ring, sqe->a);
  case KIRQ_ACK:
    return ack(ring, sqe->a, sqe->b);
  case KIRQ_MSI:
    return allocate_msi(curr, sqe);
  case KIRQ_BIND:
    return bind_msi(curr->proc, ring, sqe->a, sqe->b);
  default:
    return SYSERR_NOSYS;
  }
}

void irq_replay(struct ring *ring) {
  for (struct irq_route *route = ring->irq_claims; route != nullptr;
       route = route->next_claim) {
    spinlock_lock(&route->lock);
    if (route->ring == ring)
      post_pending_locked(route);
    spinlock_unlock(&route->lock);
  }
}

void irq_endpoint_destroy(struct ring *ring) {
  while (ring->irq_claims != nullptr) {
    struct irq_route *route = ring->irq_claims;
    spinlock_lock(&route->lock);
    if (route->present)
      x86_ioapic_mask(route->gsi);
    route->ring = nullptr;
    route->posted = false;
    spinlock_unlock(&route->lock);
    ring->irq_claims = route->next_claim;
    route->next_claim = nullptr;
    if (!route->present) {
      route->allocated = false;
      route->target = nullptr;
    }
    ring->nclaims--;
  }
}

bool irq_reap_one_locked(struct process *p) {
  for (uint32_t i = 0; i < IRQ_ROUTE_COUNT; i++) {
    struct irq_route *route = &g_routes[i];
    spinlock_lock(&route->lock);
    if (route->allocated && route->target == p && route->ring == nullptr) {
      route->allocated = false;
      route->target = nullptr;
      spinlock_unlock(&route->lock);
      return true;
    }
    spinlock_unlock(&route->lock);
  }
  return false;
}
