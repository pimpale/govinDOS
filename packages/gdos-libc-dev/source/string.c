#include "string.h"

size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len] != '\0') {
    len++;
  }
  return len;
}

int strcmp(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

void *memset(void *ptr, int value, size_t size) {
  unsigned char *dst = ptr;
  for (size_t i = 0; i < size; i++) {
    dst[i] = (unsigned char)value;
  }
  return ptr;
}

void *memcpy(void *dst, const void *src, size_t size) {
  unsigned char *dst_bytes = dst;
  const unsigned char *src_bytes = src;
  for (size_t i = 0; i < size; i++) {
    dst_bytes[i] = src_bytes[i];
  }
  return dst;
}

void *memmove(void *dest, const void *src, size_t n) {
  unsigned char *dest_bytes = dest;
  const unsigned char *src_bytes = src;

  if (src_bytes < dest_bytes) {
    for (size_t i = n; i > 0; i--) {
      dest_bytes[i - 1] = src_bytes[i - 1];
    }
  } else if (src_bytes > dest_bytes) {
    for (size_t i = 0; i < n; i++) {
      dest_bytes[i] = src_bytes[i];
    }
  }
  return dest;
}
