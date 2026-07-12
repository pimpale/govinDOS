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

// The boot identity mapping's flags: present, kernel-only, RWX, write-back.
// Pristinity invariant: anything returned to the buddy allocator must be
// mapped exactly like this in every live address space first — frees
// *restore* this mapping rather than unmapping, which lets the merge pass
// in as_flag collapse the region back into the surrounding hugepages.
#define PAGE_KERNEL_PRISTINE (PAGE_R | PAGE_W | PAGE_X)

// ---------------------------------------------------------------------------
// Lifecycle / singletons
// ---------------------------------------------------------------------------

// The kernel address space. Set during BSP bring-up (cpu_setup.c) and
// boot-static after it: the only post-boot skeleton mutations were
// kthread-stack guard punches, and kernel threads no longer exist. User
// process address spaces are cloned directly from it.
extern struct address_space *g_as_kernel;

/////////////////////////////////////////////////////////////
// AS Data Manipulation (No side effects apart from allocation/deallocation)
/////////////////////////////////////////////////////////////

// Create an identity mapped address space.
struct address_space *as_identity_mapping(void);

// create a deep copy of an address space
struct address_space *as_clone(struct address_space* src);

// Frees the address space and its children. The AS must be unpinned.
void as_free(struct address_space *as);

// Pin an address space against as_free: teardown paths that flush a
// range out of a foreign AS *after* dropping the locks that made the AS
// reachable hold a pin across the flush. as_free is never called on a
// pinned AS — the reaper treats pins != 0 as "still draining" (retry).
void as_pin(struct address_space *as);
void as_unpin(struct address_space *as);
bool as_has_pins(const struct address_space *as);

// get data about an address in a given tab
int as_getinfo(const struct address_space *as, uint64_t addr,
               paging_flags_t *flags_out, bool *present_out);

// Number of page-table pages (all levels, including the root) owned by
// this address space. Debug/testing aid: flag-and-revert sequences should
// return the count to its prior value if the merge pass is working.
uint64_t as_table_count(struct address_space *as);

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

// Flush n address spaces in ONE cross-CPU shootdown round: each AS's
// dirty range is snapshotted and cleared, and a single IPI round
// invalidates the union range on every CPU running any of them. This is
// what keeps multi-view revocation O(1) rounds instead of O(sharers).
int as_flush_multi(struct address_space *const *ases, size_t n);

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

// Service the in-flight shootdown request targeting this CPU, if any.
// Idempotent and cheap. Any IRQs-off spin loop that can run while a
// shootdown initiator waits for this CPU's ack MUST call this each
// iteration (the initiator's IPI cannot be delivered with IRQs off) —
// e.g. a lock that is ever held across as_flush by another CPU.
void paging_service_shootdown(void);

#endif // paging_h_INCLUDED
