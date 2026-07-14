#ifndef sys_h_INCLUDED
#define sys_h_INCLUDED

// gdoslib conveniences built on the public, header-only syscall wrappers.

#include <stdint.h>

#include <gdos/sys.h>
#include <string.h>

static inline void print(const char *s) { sys_debug_write(s, strlen(s)); }

static inline void print_hex(uint64_t v) {
  char buf[17];
  for (int i = 15; i >= 0; i--) {
    uint64_t d = v & 0xf;
    buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  buf[16] = '\n';
  sys_debug_write(buf, sizeof(buf));
}

// The image's load address == the base of the ublock the loader put it
// in, so this is what VM_SHARE wants. lld-link provides the
// pseudo-symbol per binary.
extern char __ImageBase;

#endif // sys_h_INCLUDED
