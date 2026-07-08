#ifndef usys_h_INCLUDED
#define usys_h_INCLUDED

// lib/sys: the OS interface — syscall stubs and ring-3 conveniences on
// top of the shared ABI headers (abi/gdos/, where the numbers, error
// values, and kring layout live). Nothing here is mirrored from the
// kernel; both sides include the same contract.

#include <stdint.h>

#include <gdos/kring.h>
#include <gdos/syscall.h>

#include "string.h"

// Event CQE types are (1 << 63) | n; the low byte is what event-matching
// code compares against.
#define KEV_LO(ev) ((uint64_t)((ev) & 0xFF))

// SYSCALL ABI (gdos/syscall.h): rax = nr, args in r10/rdx/r8/r9, result
// in rax. rcx and r11 are clobbered by the instruction itself; the
// kernel preserves every other register.
static inline uint64_t sys4(uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3) {
  uint64_t ret;
  register uint64_t r10 __asm__("r10") = a0;
  register uint64_t rdx __asm__("rdx") = a1;
  register uint64_t r8 __asm__("r8") = a2;
  register uint64_t r9 __asm__("r9") = a3;
  __asm__ volatile("syscall"
                   : "=a"(ret), "+r"(r10), "+r"(rdx), "+r"(r8), "+r"(r9)
                   : "a"(nr)
                   : "rcx", "r11", "memory");
  return ret;
}

static inline uint64_t sys3(uint64_t nr, uint64_t a0, uint64_t a1,
                            uint64_t a2) {
  return sys4(nr, a0, a1, a2, 0);
}
static inline uint64_t sys2(uint64_t nr, uint64_t a0, uint64_t a1) {
  return sys3(nr, a0, a1, 0);
}
static inline uint64_t sys1(uint64_t nr, uint64_t a0) { return sys2(nr, a0, 0); }
static inline uint64_t sys0(uint64_t nr) { return sys2(nr, 0, 0); }

// Debug-console output (SYS_DEBUG_WRITE — serial, until a video server
// exists).
static inline void print(const char *s) {
  sys2(SYS_DEBUG_WRITE, (uint64_t)s, strlen(s));
}

static inline void print_hex(uint64_t v) {
  char buf[17];
  for (int i = 15; i >= 0; i--) {
    uint64_t d = v & 0xF;
    buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  buf[16] = '\n';
  sys2(SYS_DEBUG_WRITE, (uint64_t)buf, 17);
}

// The image's load address == the base of the ublock the loader put it
// in, so this is what VM_SHARE wants. lld-link provides the
// pseudo-symbol per binary.
extern char __ImageBase;

#endif // usys_h_INCLUDED
