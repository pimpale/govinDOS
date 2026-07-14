#ifndef cpio_h_INCLUDED
#define cpio_h_INCLUDED

// Minimal cpio "newc" (SVR4, no CRC — magic 070701; 070702 accepted and
// its checksum ignored) reader for the initfs. Members are 4-byte
// aligned, so callers get a pointer into the archive, not a page-shared
// mapping (docs/technical/boot-init-design.md §0 on why that's fine).

#include <stdint.h>

// Find the member named `name` (exactly as archived, no leading "./").
// On success returns 0 and points *data/*size into the archive. Returns
// -1 on not-found or malformed archive.
int cpio_find(const uint8_t *archive, uint64_t archive_len, const char *name,
              const uint8_t **data, uint64_t *size);

#endif // cpio_h_INCLUDED
