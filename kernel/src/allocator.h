#ifndef allocator_h_INCLUDE
#define allocator_h_INCLUDE

#include <stdint.h>
#include <efi/efi.h>
#include "buddy_allocator/buddy_allocator.h"
#include "spinlock.h"

// a (global) pointer to the allocator
extern struct buddy_allocator_s* g_allocator;
extern struct spinlock g_allocator_lock;

// require that the global allocator has been defined
void allocator_require(void);

// Build a buddy allocator covering all EFI_CONVENTIONAL_MEMORY pages in the
// given EFI memory map and install it as the global heap. Must be called once
// during kernel init before any allocation.
void allocator_init(uint64_t n_mmap, const struct efi_memory_descriptor *mmap);

// very basic early malloc and free which doesn't lock. Users must ensure they are the only one using it
void* malloc_unlocked(size_t size);
void free_unlocked(void* ptr);

// ---------------------------------------------------------------------------
// Frame metadata
// ---------------------------------------------------------------------------
//
// One entry per 4 KiB physical frame, indexed by PFN (physical address >> 12).
// The array is allocated as part of allocator_init and lives in kernel memory
// for the life of the system.
//
// The buddy allocator already tracks whether a frame is free or busy; this
// array adds *what* the busy frames are being used for and (for shareable
// frames) a refcount.

enum frame_kind : uint8_t {
    FRAME_FREE = 0,      // owned by the buddy, available for allocation
    FRAME_RESERVED,      // EFI marked non-conventional; never give out
    FRAME_KERNEL,        // permanently owned by the kernel (image, allocator,
                         //   page tables of the sealed kernel AS, etc.)
    FRAME_PAGETABLE,     // a page table belonging to some address space
    FRAME_USER,          // backing a process address space (single owner)
    FRAME_SHARED,        // mapped in multiple address spaces; refcount > 1
    FRAME_MMIO,          // a device MMIO frame (kernel-mapped, UC)
};

struct frame_info {
    uint32_t       refcount;   // number of PTEs pointing here; meaningful for
                               //   FRAME_USER / FRAME_SHARED / FRAME_PAGETABLE
    enum frame_kind kind;
    uint8_t        flags;      // reserved for future use
    uint16_t       owner_id;   // reserved for future use (process / AS short id)
};

// Global table sized at allocator_init. Indexed by PFN.
extern struct frame_info *g_frames;
extern uint64_t           g_n_frames;

// Look up the metadata entry for a physical address. Returns NULL if the PA
// is outside the tracked range.
struct frame_info *frame_for(uint64_t pa);

// Atomically increment / decrement the refcount of the frame at `pa`.
// Returns the new value. Callers that observe a zero return from frame_put
// are responsible for actually returning the frame to the buddy.
unsigned frame_get(uint64_t pa);
unsigned frame_put(uint64_t pa);


#endif // allocator_h_INCLUDE
