#ifndef gdos_bootinfo_h_INCLUDED
#define gdos_bootinfo_h_INCLUDED

#include <stdint.h>

// The bootinfo block (docs/technical/boot-init-design.md §3): everything
// only discoverable before exit_boot_services, handed to init as one
// read-only ublock whose base is init's sole entry argument (rcx).
//
// Shared verbatim between kernel and userspace (both builds add
// -I../abi; see docs/technical/source-tree.md) — one producer, one
// consumer, same repo, same commit, so this is a versioned plain
// struct, not TLV. Deliberately absent: the
// initfs (linked into init.exe, found via bootfs_start/bootfs_end
// symbols) and a command line (no use case yet).

#define BOOTINFO_MAGIC 0x00544F4F42564F47ULL // "GOVBOOT\0" packed LE
#define BOOTINFO_VERSION 1

// fb_format values (EFI_GRAPHICS_PIXEL_FORMAT, frozen here so userspace
// doesn't need the EFI headers).
#define BOOTINFO_FB_RGBX8 0
#define BOOTINFO_FB_BGRX8 1
#define BOOTINFO_FB_BITMASK 2
#define BOOTINFO_FB_NONE 0xFFFFFFFFu

struct bootinfo {
  uint64_t magic;   // BOOTINFO_MAGIC
  uint32_t version; // BOOTINFO_VERSION
  uint32_t length;  // sizeof(struct bootinfo) as written

  // ACPI: physical address of the RSDP (identity == virtual in the
  // SASOS), 0 if the firmware exposed none.
  uint64_t acpi_rsdp;

  // GOP framebuffer at the mode the firmware left it in. fb_base == 0 /
  // fb_format == BOOTINFO_FB_NONE when there is no GOP. The framebuffer
  // is NOT mapped into init; this is discovery data for a future video
  // driver, which must ask for a mapping.
  uint64_t fb_base;
  uint64_t fb_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_pixels_per_scanline;
  uint32_t fb_format;

  // Memory at handoff (informational; authority over memory stays with
  // the kernel allocator + quota model). Page counts of the EFI map:
  // every descriptor, and the EFI_CONVENTIONAL_MEMORY subset.
  uint64_t mem_total_pages;
  uint64_t mem_usable_pages;
};

#endif // gdos_bootinfo_h_INCLUDED
