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
  bool present;
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
  if (!route->present || route->ring == nullptr) {
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
  struct irq_route *route = route_for_gsi(gsi);
  if (route == nullptr)
    return SYSERR_INVAL;
  spinlock_lock(&route->lock);
  if (route->ring != ring) {
    spinlock_unlock(&route->lock);
    return SYSERR_INVAL;
  }
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
  return 0;
}

static uint64_t ack(struct ring *ring, uint64_t gsi, uint64_t seq) {
  struct irq_route *route = route_for_gsi(gsi);
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

uint64_t irq_exec(struct thread *curr, struct ring *ring,
                  const struct ksqe *sqe) {
  (void)curr; // IRQ operations depend only on the ring's claimed routes
  switch (sqe->op) {
  case KIRQ_CLAIM:
    return claim(ring, sqe->a, sqe->b);
  case KIRQ_RELEASE:
    return release(ring, sqe->a);
  case KIRQ_ACK:
    return ack(ring, sqe->a, sqe->b);
  case KIRQ_MSI:
    return SYSERR_NOSYS; // phase 2 requires device-memory delegation
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
    x86_ioapic_mask(route->gsi);
    route->ring = nullptr;
    route->posted = false;
    spinlock_unlock(&route->lock);
    ring->irq_claims = route->next_claim;
    route->next_claim = nullptr;
    ring->nclaims--;
  }
}
