#ifndef paging_h_INCLUDED
#define paging_h_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096

// Architecture-independent identity page-tree manipulation API.
//
// All address-space mutations go through this interface. The boot-time
// constructors live in archsrc/<arch>/paging_init.h.
//
// Identity-mapping convention: every operation takes a single address
// that is simultaneously the virtual and physical address. There is no
// separate VA argument because VA == PA in this kernel.

// Opaque address space. Concrete definition is in archsrc/<arch>/paging.c.
struct address_space;

// Per-PTE intent the API takes. Architecture-specific code translates
// these to hardware PTE bits.
//
//   PAGE_R, PAGE_W, PAGE_X     - access rights granted to the mapping.
//   PAGE_U                     - usable from ring 3 (DPL=3 on x86,
//                                 EL0 on aarch64). Without it, the
//                                 mapping is kernel-only.
//   PAGE_WB / UC / WC / WT     - memory type. Default (no bit set) is
//                                 write-back, suitable for RAM. UC for
//                                 MMIO, WC for framebuffers.
typedef uint32_t paging_flags_t;
#define PAGE_R (1u << 0)
#define PAGE_W (1u << 1)
#define PAGE_X (1u << 2)
#define PAGE_U (1u << 3)

#define PAGE_WB (0u << 4)
#define PAGE_UC (1u << 4)
#define PAGE_WC (2u << 4)
#define PAGE_WT (3u << 4)
#define PAGE_CACHE_MASK (3u << 4)

// ---------------------------------------------------------------------------
// Lifecycle / singletons
// ---------------------------------------------------------------------------

// The kernel address space.
// will be set in cpu_setup.c
extern struct address_space *g_as_kernel;

/////////////////////////////////////////////////////////////
// AS Data Manipulation (No side effects apart from allocation/deallocation)
/////////////////////////////////////////////////////////////

// Create an identity mapped address space.
struct address_space *as_identity_mapping(void);

// create a deep copy of an address space
struct address_space *as_clone(struct address_space* src);

// Frees the address space and its children
void as_free(struct address_space *as);

// get data about an address in a given tab
int as_getinfo(const struct address_space *as, uint64_t addr,
               paging_flags_t *flags_out, bool *present_out);

// set flags
// will mark ranges as dirty. Call flush after making a set of changes
int as_flag(struct address_space *as, uint64_t addr, uint64_t end,
            paging_flags_t flags);

//////////////////////////////////////////////////////
// Invalidation + Shootdown Functions (Yes side effects)
//////////////////////////////////////////////////////

// Make `as` the current address space on this CPU. Updates
// g_as_current_percpu[this_cpu] and writes the architecture's page-table
// base register (CR3 on x86_64).
void as_switch(struct address_space *as);

// invalidate dirty pages
int as_flush(struct address_space *as);

//////////////////////////////////////////////////////
// Cross-CPU shootdown (interrupt dispatch glue)
//////////////////////////////////////////////////////

// IDT vector reserved for TLB shootdown IPIs. Architecture-specific dispatch
// (interrupts.c on x86_64) routes this vector to paging_handle_tlb_shootdown.
#define VECTOR_TLB_SHOOTDOWN 0xFD

// ISR body for the TLB shootdown IPI. Reads the in-flight shootdown
// request, invalidates the requested range locally if this CPU is running
// the targeted AS, acks the initiator, and EOIs the local APIC.
void paging_handle_tlb_shootdown(void);

#endif // paging_h_INCLUDED
