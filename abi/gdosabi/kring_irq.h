#ifndef gdos_kring_irq_h_INCLUDED
#define gdos_kring_irq_h_INCLUDED

#include <gdosabi/kring.h>

// Scheme -4: exclusive hardware interrupt claims.
//
// "gsi" below names an entry in the kernel's route table. For pin IRQs
// it IS the ACPI GSI. KIRQ_MSI (phase 2) returns a pseudo-gsi — a route
// entry with no interrupt-controller pin behind it — that feeds
// KIRQ_ACK/KIRQ_RELEASE exactly like a real one.
#define KSCHEME_IRQ ((int64_t)-4)

// SQE ops.
#define KIRQ_CLAIM   1 // a = token offset, b = token length, c = event cookie
#define KIRQ_RELEASE 2 // a = GSI (or MSI pseudo-gsi)
#define KIRQ_ACK     3 // a = GSI (or MSI pseudo-gsi), b = serviced sequence
#define KIRQ_MSI     4 // a = parent token offset, b = parent token length,
                       // c = child-token output offset; completion a = route id
#define KIRQ_MSI_ADDR 5 // a = concrete token offset, b = token length;
                        // completion a = MSI address,
                        // b = packed route-id/data

#define KIRQ_MSI_PACK(route, data)                                           \
  (((uint64_t)(uint32_t)(route) << 32) | (uint32_t)(data))
#define KIRQ_MSI_ROUTE(v) ((uint32_t)((v) >> 32))
#define KIRQ_MSI_DATA(v)  ((uint32_t)(v))

// Event CQE types.
#define KEV_IRQ KEV(5) // a = cookie, b = raise sequence

#endif // gdos_kring_irq_h_INCLUDED
