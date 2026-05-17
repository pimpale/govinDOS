#ifndef paging_h_INCLUDED
#define paging_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

// Architecture-independent page-tree manipulation API.
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
//                                 PAGE_R is the minimum for any present
//                                 mapping; absence of all three means
//                                 "not present" (use as_unmap instead).
//   PAGE_U                     - usable from ring 3 (DPL=3 on x86,
//                                 EL0 on aarch64). Without it, the
//                                 mapping is kernel-only.
//   PAGE_WB / UC / WC / WT     - memory type. Default (no bit set) is
//                                 write-back, suitable for RAM. UC for
//                                 MMIO, WC for framebuffers.
typedef uint32_t paging_flags_t;
#define PAGE_R   (1u << 0)
#define PAGE_W   (1u << 1)
#define PAGE_X   (1u << 2)
#define PAGE_U   (1u << 3)

#define PAGE_WB  (0u << 4)
#define PAGE_UC  (1u << 4)
#define PAGE_WC  (2u << 4)
#define PAGE_WT  (3u << 4)
#define PAGE_CACHE_MASK (3u << 4)

// ---------------------------------------------------------------------------
// Lifecycle / singletons
// ---------------------------------------------------------------------------

// The kernel address space. Built by paging_init_shared() and sealed
// against further modification once boot finishes.
struct address_space *as_kernel(void);

// Bootstrap the kernel AS's root table. Called once from
// paging_init_shared, before any other operation on the kernel AS.
// Not for general use.
void as_init_kernel(void);

// Create a new (unsealed) process address space. Aliases the kernel
// AS's upper-level tables by reference where possible; private tables
// are allocated lazily on first per-process upgrade.
struct address_space *as_create(void);

// Tear down a non-sealed AS. Frees every private intermediate table
// owned by `as` and drops refcounts on every user frame it has mapped.
// Must not be called on the kernel AS.
void as_destroy(struct address_space *as);

// One-way: mark `as` as immutable. Any later as_map / as_unmap /
// as_protect on it asserts. Used to enforce "kernel mappings are
// frozen after boot" as a structural invariant.
void as_seal(struct address_space *as);

// ---------------------------------------------------------------------------
// Per-CPU operations
// ---------------------------------------------------------------------------

// Switch this CPU to `as`. Updates the AS's active-CPU mask for
// future shootdowns.
void as_switch(struct address_space *as);

// Invalidate the TLB for `addr` on this CPU, then IPI-shootdown to
// any other CPU currently running `as`. Called automatically by the
// map/unmap/protect operations; exposed for code that wants to batch
// several mutations and flush once.
void as_flush(struct address_space *as, uint64_t addr);

// ---------------------------------------------------------------------------
// Tree manipulation
// ---------------------------------------------------------------------------
//
// addr is both VA and PA (identity-mapped). All addrs must be 4 KiB
// aligned. Range variants take a half-open [start, end).
//
// Return value: 0 on success, negative on failure (out of memory,
// already-mapped, etc. -- specific codes TBD).

int as_map     (struct address_space *as, uint64_t addr, paging_flags_t flags);
int as_unmap   (struct address_space *as, uint64_t addr);
int as_protect (struct address_space *as, uint64_t addr, paging_flags_t flags);
int as_walk    (const struct address_space *as, uint64_t addr,
                paging_flags_t *flags_out, bool *present_out);

// Bulk variants. The implementation is allowed to install huge-page
// leaves (2 MiB / 1 GiB) when the range is large and aligned enough,
// which is the only practical way to build the kernel identity map.
int as_map_range  (struct address_space *as,
                   uint64_t start, uint64_t end, paging_flags_t flags);
int as_unmap_range(struct address_space *as,
                   uint64_t start, uint64_t end);

#endif // paging_h_INCLUDED
