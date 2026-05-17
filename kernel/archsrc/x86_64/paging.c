#include "paging.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "buddy_allocator/buddy_allocator.h"
#include "debug.h"
#include "lapic.h"
#include "spinlock.h"
#include "stdlib/stdlib.h"
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

// Levels are named by the table they index into:
//   4 = PML4   (covers 512 GiB per entry)
//   3 = PDPT   (covers   1 GiB per entry; leaf if PS=1)
//   2 = PD     (covers   2 MiB per entry; leaf if PS=1)
//   1 = PT     (covers   4 KiB per entry; always leaf)
static const int      LEVEL_SHIFT[5] = {0, 12, 21, 30, 39};
static const uint64_t LEVEL_SIZE[5]  = {0, PAGE_SIZE, 2 * MIB, GIB, 512 * GIB};

static inline size_t level_idx(uint64_t addr, int level) {
    return (addr >> LEVEL_SHIFT[level]) & 0x1ff;
}

// ---------------------------------------------------------------------------
// Address space + globals
// ---------------------------------------------------------------------------

// Sentinel: min_dirty > max_dirty means "no dirty range tracked".
#define DIRTY_EMPTY_MIN  UINT64_MAX
#define DIRTY_EMPTY_MAX  0

struct address_space {
    pte_t   *root;
    uint64_t min_dirty;     // lowest dirty VA (inclusive)
    uint64_t max_dirty;     // highest dirty VA (exclusive)
};

// xAPIC LAPIC IDs are 8-bit; enumerate_cpus already excludes IDs > 254.
#define MAX_CPUS 256

struct address_space  *g_as_kernel = nullptr;
static struct address_space *current_per_cpu_storage[MAX_CPUS] = { nullptr };
struct address_space **g_as_current_percpu = current_per_cpu_storage;

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

    struct frame_info *fi = frame_for((uint64_t)p);
    if (fi != nullptr) {
        fi->kind     = FRAME_PAGETABLE;
        fi->refcount = 1;
    }
    return p;
}

static void pt_free(pte_t *p) {
    struct frame_info *fi = frame_for((uint64_t)p);
    if (fi != nullptr) {
        fi->kind     = FRAME_FREE;
        fi->refcount = 0;
    }
    buddy_page_free(g_allocator, (uint64_t)p / PAGE_SIZE);
}

// ---------------------------------------------------------------------------
// Flag translation
// ---------------------------------------------------------------------------

// Convert API flags to the hardware PTE bits for a leaf, excluding the PA
// field and PTE_PS. Returns 0 if `f` is empty — meaning "absent / guard".
static pte_t leaf_flags_to_pte(paging_flags_t f) {
    if ((f & (PAGE_R | PAGE_W | PAGE_X | PAGE_U)) == 0) {
        return 0;
    }
    asserts((f & PAGE_R) != 0,
            "paging: non-empty flags must include PAGE_R\n");
    pte_t pte = PTE_P;
    if (f & PAGE_W)    pte |= PTE_RW;
    if (f & PAGE_U)    pte |= PTE_US;
    if (!(f & PAGE_X)) pte |= PTE_NX;
    switch (f & PAGE_CACHE_MASK) {
        case PAGE_WB:                       break;
        case PAGE_UC: pte |= PTE_PCD;       break;
        case PAGE_WT: pte |= PTE_PWT;       break;
        case PAGE_WC: pte |= PTE_PWT;       break;  // default PAT: PWT alone == WC-ish
        default:      fatal("paging: unknown cache type\n");
    }
    return pte;
}

// Recover API flags from a present leaf PTE (for as_getinfo).
static paging_flags_t pte_to_flags(pte_t pte) {
    if (!(pte & PTE_P)) return 0;
    paging_flags_t f = PAGE_R;
    if (pte & PTE_RW)   f |= PAGE_W;
    if (pte & PTE_US)   f |= PAGE_U;
    if (!(pte & PTE_NX)) f |= PAGE_X;
    if      (pte & PTE_PCD) f |= PAGE_UC;
    else if (pte & PTE_PWT) f |= PAGE_WT;
    else                     f |= PAGE_WB;
    return f;
}

// Intermediate (non-leaf) entries are deliberately permissive: P|RW|US.
// Effective access is the AND of every level's bits, so the leaf flags
// are what actually matter. Permissive intermediates let one tree carry
// both U=0 kernel leaves and U=1 user leaves without restructuring.
static pte_t intermediate_flags(void) {
    return PTE_P | PTE_RW | PTE_US;
}

// ---------------------------------------------------------------------------
// Dirty tracking
// ---------------------------------------------------------------------------

static void mark_dirty(struct address_space *as, uint64_t start, uint64_t end) {
    if (start < as->min_dirty) as->min_dirty = start;
    if (end   > as->max_dirty) as->max_dirty = end;
}

static bool has_dirty(const struct address_space *as) {
    return as->min_dirty < as->max_dirty;
}

static void clear_dirty(struct address_space *as) {
    as->min_dirty = DIRTY_EMPTY_MIN;
    as->max_dirty = DIRTY_EMPTY_MAX;
}

// ---------------------------------------------------------------------------
// Walk + install
// ---------------------------------------------------------------------------

static void free_table(int level, pte_t *table);

// Replace a huge leaf with a freshly populated table of one-level-smaller
// leaves of identical translation. `level` is the level of the huge being
// split (3 splits a 1 GiB huge into 512×2 MiB hugies in a PD; 2 splits a
// 2 MiB huge into 512×4 KiB PTEs in a PT). Flag bits are preserved verbatim
// except for PS, which is set on the children only when they are still huge
// — at the PT level bit 7 is PAT, not PS.
static pte_t *split_huge(pte_t huge, int level) {
    asserts(level == 2 || level == 3, "split_huge: invalid level\n");
    asserts((huge & PTE_P) && (huge & PTE_PS),
            "split_huge: entry is not a huge leaf\n");
    pte_t   *child    = pt_alloc();
    uint64_t base     = huge & PTE_ADDR;
    pte_t    flags    = (huge & ~PTE_ADDR) & ~PTE_PS;
    pte_t    child_ps = (level - 1 == 2) ? PTE_PS : 0;
    uint64_t step     = LEVEL_SIZE[level - 1];
    for (size_t i = 0; i < 512; i++) {
        child[i] = (base + i * step) | flags | child_ps;
    }
    return child;
}

// Install `leaf_pte` at the chosen level (1 = 4 KiB, 2 = 2 MiB huge,
// 3 = 1 GiB huge). `leaf_pte == 0` means "clear / absent" and is handled
// without allocating intermediate tables.
//
// Coarser hugies along the walk are demoted on touch so the sub-range can
// be addressed; finer sub-trees at the target slot are freed when a coarser
// leaf is being installed over them, to avoid leaking page-table frames.
static void install_leaf(struct address_space *as, uint64_t addr, int level,
                         pte_t leaf_pte) {
    asserts(level >= 1 && level <= 3, "paging: invalid leaf level\n");
    asserts((addr & (LEVEL_SIZE[level] - 1)) == 0,
            "paging: leaf address not aligned to its level\n");

    pte_t *table = as->root;
    for (int l = 4; l > level; l--) {
        size_t i = level_idx(addr, l);
        pte_t  e = table[i];
        if (!(e & PTE_P)) {
            if (leaf_pte == 0) return;        // nothing to clear, no need to allocate
            pte_t *child = pt_alloc();
            table[i] = (uint64_t)child | intermediate_flags();
            table = child;
        } else if (e & PTE_PS) {
            // Huge coarser than our target: split it. The new sub-table
            // preserves the original translation for every entry, but TLBs
            // that cached the huge are now stale relative to its new
            // granularity — widen the dirty range so the next flush evicts.
            pte_t *child = split_huge(e, l);
            table[i] = (uint64_t)child | intermediate_flags();
            uint64_t huge_base = addr & ~(LEVEL_SIZE[l] - 1);
            mark_dirty(as, huge_base, huge_base + LEVEL_SIZE[l]);
            table = child;
        } else {
            table = (pte_t *)(e & PTE_ADDR);
        }
    }

    // About to overwrite the target slot. If it currently points at a
    // finer-grained sub-tree (present, not PS), free that sub-tree first
    // so the page-table frames are returned to the buddy.
    size_t li   = level_idx(addr, level);
    pte_t  prev = table[li];
    if ((prev & PTE_P) && !(prev & PTE_PS) && level > 1) {
        free_table(level - 1, (pte_t *)(prev & PTE_ADDR));
    }
    table[li] = leaf_pte;
}

// Read-only walk. Returns the leaf PTE encountered (or 0 if absent at any
// level). The level at which the leaf was found isn't reported — callers
// only care about the flag bits and presence.
static pte_t walk(const pte_t *root, uint64_t addr) {
    const pte_t *table = root;
    for (int l = 4; l >= 1; l--) {
        size_t i = level_idx(addr, l);
        pte_t  e = table[i];
        if (!(e & PTE_P)) return 0;
        if (l == 1 || (e & PTE_PS)) return e;
        table = (const pte_t *)(e & PTE_ADDR);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Recursive tree helpers (clone + free)
// ---------------------------------------------------------------------------

// Deep-copy a table at `level`. For each entry: absent or leaf (huge / level 1)
// copies the 8-byte PTE verbatim; intermediate entries recurse and the child
// pointer in the destination is rewritten to the new table.
static pte_t *clone_table(int level, const pte_t *src) {
    pte_t *dst = pt_alloc();
    for (size_t i = 0; i < 512; i++) {
        pte_t e = src[i];
        if (!(e & PTE_P)) { dst[i] = 0; continue; }
        if (level == 1 || (e & PTE_PS)) {
            dst[i] = e;
            continue;
        }
        pte_t *child = clone_table(level - 1, (const pte_t *)(e & PTE_ADDR));
        dst[i] = (uint64_t)child | (e & ~PTE_ADDR);
    }
    return dst;
}

// Recursive free. Walks every intermediate table this AS owns and returns
// it to the buddy. Leaf frames (the physical pages the leaves point at) are
// NOT freed here — that's a separate concern handled by frame refcounts.
static void free_table(int level, pte_t *table) {
    if (level > 1) {
        for (size_t i = 0; i < 512; i++) {
            pte_t e = table[i];
            if (!(e & PTE_P) || (e & PTE_PS)) continue;
            free_table(level - 1, (pte_t *)(e & PTE_ADDR));
        }
    }
    pt_free(table);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

struct address_space *as_identity_mapping(void) {
    struct address_space *as = malloc(sizeof(*as));
    asserts(as != nullptr, "paging: failed to allocate address_space\n");
    as->root      = pt_alloc();
    as->min_dirty = DIRTY_EMPTY_MIN;
    as->max_dirty = DIRTY_EMPTY_MAX;

    // Identity map the first 512 GiB with 1 GiB huge pages. Flags: R|W|X,
    // kernel-only, WB. NX is deliberately *not* set because the kernel
    // image's .text needs to be executable and we don't yet split sections
    // out. Revisit when we want a stricter kernel mapping.
    pte_t *pdpt = pt_alloc();
    pte_t  leaf_base = leaf_flags_to_pte(PAGE_R | PAGE_W | PAGE_X);
    for (size_t i = 0; i < 512; i++) {
        pdpt[i] = (i * GIB) | leaf_base | PTE_PS;
    }
    as->root[0] = (uint64_t)pdpt | intermediate_flags();
    return as;
}

struct address_space *as_clone(struct address_space *src) {
    struct address_space *as = malloc(sizeof(*as));
    asserts(as != nullptr, "paging: failed to allocate address_space\n");
    as->root      = clone_table(4, src->root);
    as->min_dirty = DIRTY_EMPTY_MIN;
    as->max_dirty = DIRTY_EMPTY_MAX;
    return as;
}

void as_free(struct address_space *as) {
    free_table(4, as->root);
    free(as);
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

int as_getinfo(const struct address_space *as, uint64_t addr,
               paging_flags_t *flags_out, bool *present_out) {
    pte_t pte = walk(as->root, addr);
    bool present = (pte & PTE_P) != 0;
    if (present_out != nullptr) *present_out = present;
    if (flags_out   != nullptr) *flags_out   = present ? pte_to_flags(pte) : 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

int as_flag(struct address_space *as, uint64_t addr, uint64_t end,
            paging_flags_t flags) {
    asserts(addr % PAGE_SIZE == 0 && end % PAGE_SIZE == 0,
            "as_flag: range not page-aligned\n");
    asserts(end >= addr, "as_flag: end < start\n");
    if (addr == end) return 0;

    pte_t leaf_base = leaf_flags_to_pte(flags);
    bool  present   = (leaf_base & PTE_P) != 0;

    uint64_t a = addr;
    while (a < end) {
        uint64_t remaining = end - a;
        if ((a % GIB) == 0 && remaining >= GIB) {
            install_leaf(as, a, 3, present ? (a | leaf_base | PTE_PS) : 0);
            a += GIB;
        } else if ((a % (2 * MIB)) == 0 && remaining >= 2 * MIB) {
            install_leaf(as, a, 2, present ? (a | leaf_base | PTE_PS) : 0);
            a += 2 * MIB;
        } else {
            install_leaf(as, a, 1, present ? (a | leaf_base) : 0);
            a += PAGE_SIZE;
        }
    }

    mark_dirty(as, addr, end);
    return 0;
}

void *kernel_stack_alloc(size_t total_size) {
    asserts(g_as_kernel != nullptr,
            "kernel_stack_alloc: kernel address space is not initialized\n");

    uint64_t total_pages = total_size/PAGE_SIZE;

    uint64_t page_id = 0;
    spinlock_lock(&g_allocator_lock);
    buddy_status_t s = buddy_page_alloc(g_allocator, total_pages, &page_id);
    spinlock_unlock(&g_allocator_lock);
    asserts(s == BUDDY_STATUS_SUCCESS,
            "kernel_stack_alloc: out of memory allocating stack\n");

    uint64_t base = page_id * PAGE_SIZE;
    uint64_t top = base + 1 * PAGE_SIZE;

    memset((void *)(uintptr_t)base, 0, top - base);
    as_flag(g_as_kernel, base, base + PAGE_SIZE, 0);

    return (void *)(uintptr_t)top;
}

// ---------------------------------------------------------------------------
// Side effects: switch + flush
// ---------------------------------------------------------------------------

static uint32_t this_cpu(void) {
    return (uint32_t)x86_lapic_id();
}

void as_switch(struct address_space *as) {
    asserts(as != nullptr && as->root != nullptr,
            "paging: switching to invalid address_space\n");
    g_as_current_percpu[this_cpu()] = as;
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)as->root) : "memory");
}

// Page-granularity invalidation threshold. Below this many dirty pages,
// invlpg per page; above, write CR3 to flush all non-global entries on
// this CPU at once.
#define INVLPG_THRESHOLD_PAGES 8

// Apply the standard local-flush policy to [min, max). Used by both the
// initiator's local pass and by remote CPUs handling a shootdown IPI.
static void tlb_invalidate_local(uint64_t min, uint64_t max) {
    uint64_t span_pages = (max - min) / PAGE_SIZE;
    if (span_pages < INVLPG_THRESHOLD_PAGES) {
        for (uint64_t a = min; a < max; a += PAGE_SIZE) {
            __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
        }
    } else {
        // Writing CR3 with its current value flushes all non-global TLB
        // entries on this CPU.
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
}

// Cross-CPU TLB shootdown mailbox. Only one shootdown is in flight at a
// time (serialized by g_tlb_lock), so a single global slot is enough.
//
//   .as      : the address space whose dirty range needs invalidating.
//              Remote CPUs short-circuit if they're not actually running it.
//   .min/.max: the dirty range to invalidate.
//   .pending : ack counter; targets decrement it as they finish, the
//              initiator spins until it hits zero.
static struct {
    struct address_space *as;
    uint64_t              min;
    uint64_t              max;
    _Atomic uint32_t      pending;
} g_tlb_req;

static struct spinlock g_tlb_lock = SPINLOCK_INITIALIZER;

void paging_handle_tlb_shootdown(void) {
    uint32_t me = this_cpu();
    if (g_as_current_percpu[me] == g_tlb_req.as) {
        tlb_invalidate_local(g_tlb_req.min, g_tlb_req.max);
    }
    atomic_fetch_sub_explicit(&g_tlb_req.pending, 1, memory_order_release);
    x86_lapic_eoi();
}

int as_flush(struct address_space *as) {
    if (!has_dirty(as)) return 0;

    uint32_t me  = this_cpu();
    uint64_t min = as->min_dirty;
    uint64_t max = as->max_dirty;

    spinlock_lock(&g_tlb_lock);

    // Publish the request. Remote CPUs only consult these once their IPI
    // fires, so plain stores under the lock are sufficient — x86's TSO
    // ordering plus the LAPIC ICR write below act as the release barrier.
    g_tlb_req.as  = as;
    g_tlb_req.min = min;
    g_tlb_req.max = max;

    // Count and target every other CPU currently running this AS.
    uint32_t targets = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (i == me) continue;
        if (g_as_current_percpu[i] == as) targets++;
    }
    atomic_store_explicit(&g_tlb_req.pending, targets, memory_order_release);

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (i == me) continue;
        if (g_as_current_percpu[i] == as) {
            x86_lapic_send_fixed((uint8_t)i, VECTOR_TLB_SHOOTDOWN);
        }
    }

    // Do the local flush while remote CPUs are working.
    if (g_as_current_percpu[me] == as) {
        tlb_invalidate_local(min, max);
    }

    while (atomic_load_explicit(&g_tlb_req.pending,
                                memory_order_acquire) != 0) {
        __asm__ volatile("pause");
    }

    spinlock_unlock(&g_tlb_lock);

    clear_dirty(as);
    return 0;
}
