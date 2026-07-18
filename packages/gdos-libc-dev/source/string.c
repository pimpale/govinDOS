#include "string.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len] != '\0') {
    len++;
  }
  return len;
}

size_t strnlen(const char *s, size_t maxlen) {
  size_t len = 0;
  while (len < maxlen && s[len] != '\0')
    len++;
  return len;
}

int strcmp(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char ac = (unsigned char)a[i];
    unsigned char bc = (unsigned char)b[i];
    if (ac != bc || ac == 0)
      return ac - bc;
  }
  return 0;
}

char *strcpy(char *restrict dst, const char *restrict src) {
  char *result = dst;
  while ((*dst++ = *src++) != '\0') {}
  return result;
}

char *strncpy(char *restrict dst, const char *restrict src, size_t n) {
  char *result = dst;
  size_t i = 0;
  while (i < n && src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  while (i < n)
    dst[i++] = '\0';
  return result;
}

char *strcat(char *restrict dst, const char *restrict src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

char *strncat(char *restrict dst, const char *restrict src, size_t n) {
  char *tail = dst + strlen(dst);
  size_t i = 0;
  while (i < n && src[i] != '\0') {
    tail[i] = src[i];
    i++;
  }
  tail[i] = '\0';
  return dst;
}

char *strchr(const char *s, int c) {
  char needle = (char)c;
  do {
    if (*s == needle)
      return (char *)s;
  } while (*s++ != '\0');
  return nullptr;
}

char *strrchr(const char *s, int c) {
  const char *last = nullptr;
  char needle = (char)c;
  do {
    if (*s == needle)
      last = s;
  } while (*s++ != '\0');
  return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
  if (*needle == '\0')
    return (char *)haystack;
  size_t n = strlen(needle);
  for (; *haystack != '\0'; haystack++)
    if (*haystack == *needle && strncmp(haystack, needle, n) == 0)
      return (char *)haystack;
  return nullptr;
}

static bool char_in(const char *set, char c) {
  return strchr(set, c) != nullptr;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  while (s[n] != '\0' && char_in(accept, s[n]))
    n++;
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  while (s[n] != '\0' && !char_in(reject, s[n]))
    n++;
  return n;
}

char *strpbrk(const char *s, const char *accept) {
  size_t n = strcspn(s, accept);
  return s[n] == '\0' ? nullptr : (char *)(s + n);
}

char *strtok_r(char *restrict s, const char *restrict delim,
               char **restrict saveptr) {
  if (s == nullptr)
    s = *saveptr;
  s += strspn(s, delim);
  if (*s == '\0') {
    *saveptr = s;
    return nullptr;
  }
  char *end = s + strcspn(s, delim);
  if (*end != '\0')
    *end++ = '\0';
  *saveptr = end;
  return s;
}

char *strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *copy = malloc(len + 1);
  if (copy == nullptr)
    return nullptr;
  memcpy(copy, s, len);
  copy[len] = '\0';
  return copy;
}

char *strdup(const char *s) { return strndup(s, strlen(s)); }

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

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *ap = a;
  const unsigned char *bp = b;
  for (size_t i = 0; i < n; i++)
    if (ap[i] != bp[i])
      return ap[i] - bp[i];
  return 0;
}

void *memchr(const void *ptr, int value, size_t n) {
  const unsigned char *bytes = ptr;
  for (size_t i = 0; i < n; i++)
    if (bytes[i] == (unsigned char)value)
      return (void *)(bytes + i);
  return nullptr;
}

int strcasecmp(const char *a, const char *b) {
  while (*a != '\0' && tolower((unsigned char)*a) ==
                            tolower((unsigned char)*b)) {
    a++;
    b++;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int ac = tolower((unsigned char)a[i]);
    int bc = tolower((unsigned char)b[i]);
    if (ac != bc || ac == 0)
      return ac - bc;
  }
  return 0;
}

char *strerror(int error) {
  switch (error) {
  case 0: return "Success";
  case EPERM: return "Operation not permitted";
  case ENOENT: return "No such file or directory";
  case ESRCH: return "No such process or thread";
  case EAGAIN: return "Resource temporarily unavailable";
  case ENOMEM: return "Out of memory";
  case EACCES: return "Permission denied";
  case EFAULT: return "Bad address";
  case EBUSY: return "Resource busy";
  case EEXIST: return "File exists";
  case EINVAL: return "Invalid argument";
  case EDEADLK: return "Resource deadlock avoided";
  case ENOSYS: return "Function not implemented";
  case ENOTSUP: return "Operation not supported";
  case ETIMEDOUT: return "Operation timed out";
  default: return "Unknown error";
  }
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
