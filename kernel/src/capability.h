#ifndef capability_h_INCLUDED
#define capability_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include <gdosabi/kring_cap.h>

struct process;
struct ring;
struct irq_route;
struct thread;

typedef struct grant grant;
typedef grant *grant_ptr;

#define LLRB_NAME grant_id
#define LLRB_KEY uint64_t
#define LLRB_VALUE grant_ptr
#include <llrb/llrb.h>
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME

#define SLAB_NAME llrb_grant_id_node
#define SLAB_TYPE llrb_grant_id_node
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

#define SLAB_NAME grant
#define SLAB_TYPE grant
#include <slab/slab.h>
#undef SLAB_TYPE
#undef SLAB_NAME

// Initializes the per-boot key and empty grant index. The allocator and
// g_umem must already be initialized.
void capability_init(void);

// Creates the three wildcard boot roots for init and writes their tokens.
void capability_bootstrap(struct process *init, struct cap_token *devmem,
                          struct cap_token *irq,
                          struct cap_token *iommu);

// Ring scheme -2. Called under g_umem by the common channel executor.
uint64_t cap_exec(struct thread *curr, struct ring *ring, struct ksqe *sqe);

// Verify a token held inside a kernel ring block. Caller holds g_umem.
uint64_t cap_verify_ring_locked(struct ring *ring, uint64_t off, uint64_t len,
                                uint8_t type, grant **out);

struct irq_route *cap_irq_route(const grant *g);
uint32_t cap_iommu_requester(const grant *g);

// KIRQ_MSI materializes a concrete route grant beneath a verified parent.
// Caller holds g_umem.
uint64_t cap_create_irq_route_locked(grant *parent, struct irq_route *route,
                                     struct cap_token *out);

// Token-gated SYS_VM_MAP_DEVICE backend.
uint64_t cap_map_device(struct process *p, uint64_t token_ptr,
                        uint64_t token_len, uint32_t flags);

#endif
