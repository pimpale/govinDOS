#include "paging.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "buddy_allocator/buddy_allocator.h"
#include "debug.h"
#include "stdlib/string.h"

// ---------------------------------------------------------------------------
// Hardware PTE layout (x86_64, 4-level paging)
// ---------------------------------------------------------------------------

typedef uint64_t pte_t;

#define PTE_P    (1ull << 0)
#define PTE_RW   (1ull << 1)
#define PTE_US   (1ull << 2)
#define PTE_PWT  (1ull << 3)
#define PTE_PCD  (1ull << 4)
#define PTE_PS   (1ull << 7)            // huge page at PD (2 MiB) or PDPT (1 GiB)
#define PTE_NX   (1ull << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ull

#define GIB (1ull << 30)
#define MIB (1ull << 20)

// Levels, in walk order. We name them by the table they index into so the
// arithmetic stays readable: level 4 indexes the PML4, level 1 indexes a PT.
//   level 4: PML4 entry  - covers 512 GiB per entry
//   level 3: PDPT entry  - covers   1 GiB per entry (or 1 GiB huge if PS)
//   level 2: PD entry    - covers   2 MiB per entry (or 2 MiB huge if PS)
//   level 1: PT entry    - covers   4 KiB per entry
static const int LEVEL_SHIFT[5] = {0, 12, 21, 30, 39};

static inline size_t level_idx(uint64_t addr, int level) {
    return (addr >> LEVEL_SHIFT[level]) & 0x1ff;
}

// ---------------------------------------------------------------------------
// Address space
// ---------------------------------------------------------------------------

struct address_space {
    pte_t   *root;      // PML4. Under identity mapping, PA == this pointer.
    bool     sealed;    // after seal, all mutations assert
    uint32_t cpu_mask;  // bit i: CPU i has this AS loaded (for shootdowns)
};

// The singleton kernel AS. Its root is allocated in as_init_kernel().
static struct address_space kernel_as_storage = {
    .root     = nullptr,
    .sealed   = false,
    .cpu_mask = 0,
};

struct address_space *as_kernel(void) {
    return &kernel_as_storage;
}

// ---------------------------------------------------------------------------
// Page-table page allocation
// ---------------------------------------------------------------------------

// Pull a single zeroed 4 KiB frame from the buddy. Identity mapping means
// the returned pointer is both the VA we can write to and the PA we install
// into a parent table entry.
static pte_t *pt_alloc(void) {
    allocator_require();
    uint64_t page_id = 0;
    buddy_status_t s = buddy_page_alloc(g_allocator, 1, &page_id);
    asserts(s == BUDDY_STATUS_SUCCESS,
            "paging: out of memory allocating page table\n");
    pte_t *p = (pte_t *)(page_id * PAGE_SIZE);
    memset(p, 0, PAGE_SIZE);

    // Tag the frame as a page table so frame_info bookkeeping reflects it.
    struct frame_info *fi = frame_for((uint64_t)p);
    if (fi != nullptr) {
        fi->kind = FRAME_PAGETABLE;
        fi->refcount = 1;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Flag translation
// ---------------------------------------------------------------------------

// Build the architecture PTE bits for a leaf, exclusive of the physical
// address field and the huge-page bit (callers OR those in).
static pte_t leaf_flags_to_pte(paging_flags_t f) {
    asserts((f & PAGE_R) != 0,
            "paging: leaf must include PAGE_R (use as_unmap to clear)\n");
    pte_t pte = PTE_P;
    if (f & PAGE_W) pte |= PTE_RW;
    if (f & PAGE_U) pte |= PTE_US;
    if (!(f & PAGE_X)) pte |= PTE_NX;
    switch (f & PAGE_CACHE_MASK) {
        case PAGE_WB: break;
        case PAGE_UC: pte |= PTE_PCD; break;
        case PAGE_WC: pte |= PTE_PWT; break;             // assumes default PAT
        case PAGE_WT: pte |= PTE_PWT; break;
        default:      fatal("paging: unknown cache type\n");
    }
    return pte;
}

// Intermediate (non-leaf) entries are deliberately permissive: P|RW|US.
// The effective access at any address is the AND of every level's bits,
// so the leaf's RW/US/NX are what actually matter. Permissive intermediates
// let a single tree carry both U=0 kernel leaves and U=1 user leaves.
static pte_t intermediate_flags(void) {
    return PTE_P | PTE_RW | PTE_US;
}

// ---------------------------------------------------------------------------
// Walk + install
// ---------------------------------------------------------------------------

// Install `leaf_pte` at the given level (1 = PT 4 KiB, 2 = PD 2 MiB,
// 3 = PDPT 1 GiB). `leaf_pte` already encodes the present bit, the leaf
// flags, the physical address, and PTE_PS at levels 2 and 3.
//
// Allocates intermediate tables as needed. Asserts (for now) if a huge
// page is in the way -- demoting an existing huge mapping is not yet
// implemented and is only needed by as_unmap / as_protect on a sub-region.
static void install_leaf(struct address_space *as, uint64_t addr,
                         int level, pte_t leaf_pte) {
    asserts(!as->sealed, "paging: mutation on sealed address space\n");
    asserts(level >= 1 && level <= 3, "paging: invalid leaf level\n");
    asserts((addr & ((1ull << LEVEL_SHIFT[level]) - 1)) == 0,
            "paging: leaf address not aligned to its level\n");

    pte_t *table = as->root;
    for (int l = 4; l > level; l--) {
        size_t i = level_idx(addr, l);
        pte_t  e = table[i];
        if (!(e & PTE_P)) {
            pte_t *child = pt_alloc();
            table[i] = (uint64_t)child | intermediate_flags();
            table = child;
        } else {
            asserts(!(e & PTE_PS),
                    "paging: huge page in the way; demote-on-touch NYI\n");
            table = (pte_t *)(e & PTE_ADDR);
        }
    }
    table[level_idx(addr, level)] = leaf_pte;
}

// ---------------------------------------------------------------------------
// Public lifecycle / per-CPU
// ---------------------------------------------------------------------------

void as_init_kernel(void) {
    asserts(kernel_as_storage.root == nullptr,
            "paging: kernel AS initialized twice\n");
    kernel_as_storage.root = pt_alloc();
}

void as_seal(struct address_space *as) {
    as->sealed = true;
}

void as_switch(struct address_space *as) {
    asserts(as->root != nullptr, "paging: switching to uninitialized AS\n");
    // TODO: update cpu_mask atomically with the per-CPU id once that exists.
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)as->root) : "memory");
}

void as_flush(struct address_space *as, uint64_t addr) {
    (void)as;
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
    // TODO: IPI shootdown to other CPUs in as->cpu_mask.
}

// ---------------------------------------------------------------------------
// Tree manipulation -- the parts we actually use today
// ---------------------------------------------------------------------------

int as_map_range(struct address_space *as,
                 uint64_t start, uint64_t end,
                 paging_flags_t flags) {
    asserts(start % PAGE_SIZE == 0 && end % PAGE_SIZE == 0,
            "as_map_range: range not page-aligned\n");
    asserts(end >= start, "as_map_range: end < start\n");

    pte_t base = leaf_flags_to_pte(flags);

    uint64_t a = start;
    while (a < end) {
        uint64_t remaining = end - a;
        if ((a % GIB) == 0 && remaining >= GIB) {
            install_leaf(as, a, 3, a | base | PTE_PS);
            a += GIB;
        } else if ((a % (2 * MIB)) == 0 && remaining >= 2 * MIB) {
            install_leaf(as, a, 2, a | base | PTE_PS);
            a += 2 * MIB;
        } else {
            install_leaf(as, a, 1, a | base);
            a += PAGE_SIZE;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Tree manipulation -- not yet implemented
// ---------------------------------------------------------------------------
//
// These all require either demote-on-touch (splitting an existing huge page
// into smaller entries when a sub-region needs different flags) or the
// process-AS machinery (alias detection, copy-on-demote). Stubbed loudly
// so misuse fails fast rather than silently misbehaving.

int as_map(struct address_space *as, uint64_t addr, paging_flags_t flags) {
    (void)as; (void)addr; (void)flags;
    fatal("paging: as_map not yet implemented\n");
}

int as_unmap(struct address_space *as, uint64_t addr) {
    (void)as; (void)addr;
    fatal("paging: as_unmap not yet implemented (needs demote-on-touch)\n");
}

int as_protect(struct address_space *as, uint64_t addr, paging_flags_t flags) {
    (void)as; (void)addr; (void)flags;
    fatal("paging: as_protect not yet implemented (needs demote-on-touch)\n");
}

int as_walk(const struct address_space *as, uint64_t addr,
            paging_flags_t *flags_out, bool *present_out) {
    (void)as; (void)addr; (void)flags_out; (void)present_out;
    fatal("paging: as_walk not yet implemented\n");
}

int as_unmap_range(struct address_space *as, uint64_t start, uint64_t end) {
    (void)as; (void)start; (void)end;
    fatal("paging: as_unmap_range not yet implemented\n");
}

struct address_space *as_create(void) {
    fatal("paging: as_create not yet implemented\n");
}

void as_destroy(struct address_space *as) {
    (void)as;
    fatal("paging: as_destroy not yet implemented\n");
}
