#include "cpio.h"

#include <string.h>

#define CPIO_HDR_SIZE 110
#define CPIO_FILESIZE_OFF (6 + 6 * 8)  // 7th 8-hex-digit field
#define CPIO_NAMESIZE_OFF (6 + 11 * 8) // 12th

static uint64_t align4(uint64_t v) { return (v + 3) & ~3ull; }

static uint64_t hex8(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    uint8_t c = p[i];
    uint64_t d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      return (uint64_t)-1;
    }
    v = (v << 4) | d;
  }
  return v;
}

int cpio_find(const uint8_t *archive, uint64_t archive_len, const char *name,
              const uint8_t **data, uint64_t *size) {
  uint64_t off = 0;
  // Entries start 4-aligned (the archive starts one), so the intra-entry
  // offsets below can align relative to the entry base.
  while (off + CPIO_HDR_SIZE <= archive_len) {
    const uint8_t *h = archive + off;
    if (!(h[0] == '0' && h[1] == '7' && h[2] == '0' && h[3] == '7' &&
          h[4] == '0' && (h[5] == '1' || h[5] == '2'))) {
      return -1;
    }
    uint64_t file_size = hex8(h + CPIO_FILESIZE_OFF);
    uint64_t name_size = hex8(h + CPIO_NAMESIZE_OFF);
    if (file_size == (uint64_t)-1 || name_size == (uint64_t)-1 ||
        off + CPIO_HDR_SIZE + name_size > archive_len) {
      return -1;
    }
    const char *member = (const char *)h + CPIO_HDR_SIZE;
    if (strcmp(member, "TRAILER!!!") == 0) {
      return -1;
    }
    uint64_t data_off = align4(CPIO_HDR_SIZE + name_size);
    if (off + data_off + file_size > archive_len) {
      return -1;
    }
    if (strcmp(member, name) == 0) {
      *data = h + data_off;
      *size = file_size;
      return 0;
    }
    off += align4(data_off + file_size);
  }
  return -1;
}
