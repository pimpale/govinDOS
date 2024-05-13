#include "c_builtins.h"

void *memset(void *ptr, int value, size_t size) {
  char *dst = ptr;
  for (size_t i = 0; i < size; i++) {
    dst[i] = value;
  }
  return ptr;
}

void *memcpy(void *dst, const void *src, size_t size) {
  char *dst_c = dst;
  char *src_c = dst;
  for (size_t i = 0; i < size; i++) {
    dst_c[i] = src_c[i];
  }
  return dst;
}

// if src == dest, then we're already good (same ptr)
// if src < dest, then copy bytes backward, starting from the end of src and
// going to the beginning if src > dest, then copy bytes forward, starting from
// the beginning of src and going to the end We do this to prevent overwriting
// the area we're going to read from next
void *memmove(void *dest, const void *src, size_t n) {
  char *dest_bytes = dest;
  const char *src_bytes = src;

  if (src < dest) {
    // copy backwards
    for (size_t i_plus_one = n; i_plus_one >= 1; i_plus_one--) {
      const size_t i = i_plus_one - 1;
      // here i actually represents the next byte over to avoid overflowing
      dest_bytes[i] = src_bytes[i];
    }
  } else if (src > dest) {
    // copy forward
    for (size_t i = 0; i < n; i++) {
      dest_bytes[i] = src_bytes[i];
    }
  }
  return dest;
}
