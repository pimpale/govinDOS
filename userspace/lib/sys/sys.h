#ifndef sys_h_INCLUDED
#define sys_h_INCLUDED

// lib/sys: the general-purpose system library — raw syscall stubs
// (sys0..sys4), one typed C wrapper per syscall, and ring-3 conveniences
// on top of the shared ABI headers (abi/gdos/, where the numbers, error
// values, and kring layout live). Nothing here is mirrored from the
// kernel; both sides include the same contract. The ring-side companion
// is <kring.h> (the liburing-flavoured kernel-channel interface).

#include <stdint.h>

#include <gdos/syscall.h>

#include <string.h>

// SYSCALL ABI (gdos/syscall.h): rax = nr, args in r10/rdx/r8/r9, result
// in rax. rcx and r11 are clobbered by the instruction itself; the
// kernel preserves every other register.
static inline uint64_t sys4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3) {
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
static inline uint64_t sys1(uint64_t nr, uint64_t a0) {
  return sys2(nr, a0, 0);
}
static inline uint64_t sys0(uint64_t nr) { return sys2(nr, 0, 0); }

// SYSERR_* are small negative u64s: anything in the top page of the
// address space is an error, not a result.
static inline bool sys_iserr(uint64_t v) { return v >= (uint64_t)-4096; }

// ---------------------------------------------------------------------------
// Typed wrappers, one per syscall (gdos/syscall.h documents semantics
// and errors). All return the raw rax value; check with sys_iserr or
// against the SYSERR_* constants.
// ---------------------------------------------------------------------------

static inline uint64_t sys_debug_write(const void *buf, uint64_t len) {
  return sys2(SYS_DEBUG_WRITE, (uint64_t)buf, len);
}

[[noreturn]] static inline void sys_exit(void) {
  sys0(SYS_EXIT);
  __builtin_unreachable();
}

static inline void sys_yield(void) { sys0(SYS_YIELD); }
static inline uint64_t sys_getpid(void) { return sys0(SYS_GETPID); }

// Blocks are the allocation unit (power-of-two pages); returns the base.
static inline uint64_t sys_vm_alloc(uint64_t len, uint64_t prot) {
  return sys2(SYS_VM_ALLOC, len, prot);
}
// base alone names the block — blocks are the unit, no length needed.
static inline uint64_t sys_vm_free(uint64_t base) {
  return sys1(SYS_VM_FREE, base);
}
static inline uint64_t sys_vm_protect(uint64_t base, uint64_t len,
                                      uint64_t prot) {
  return sys3(SYS_VM_PROTECT, base, len, prot);
}
// The pid form: a parent applying view flags (W^X on a written image)
// to its own embryo — the one window of authority over another
// process's views.
static inline uint64_t sys_vm_protect_for(uint64_t base, uint64_t len,
                                          uint64_t prot, uint64_t pid) {
  return sys4(SYS_VM_PROTECT, base, len, prot, pid);
}
// target > 0: map the owned block into that process. target < 0: turn
// the block into a kernel channel of that scheme (prot ignored).
static inline uint64_t sys_vm_share(uint64_t base, int64_t target,
                                    uint64_t prot) {
  return sys3(SYS_VM_SHARE, base, (uint64_t)target, prot);
}
static inline uint64_t sys_vm_unshare(uint64_t base) {
  return sys1(SYS_VM_UNSHARE, base);
}
static inline uint64_t sys_vm_move(uint64_t base, uint64_t pid) {
  return sys2(SYS_VM_MOVE, base, pid);
}
static inline uint64_t sys_vm_map_device(uint64_t base, uint64_t len,
                                         uint64_t flags) {
  return sys3(SYS_VM_MAP_DEVICE, base, len, flags);
}

static inline uint64_t sys_block_doorbell(uint64_t base) {
  return sys1(SYS_BLOCK_DOORBELL, base);
}
// Parks while the 32-bit word at addr equals expected; addr must sit in
// a block the caller has a view of.
static inline uint64_t sys_block_wait(const volatile void *addr,
                                      uint32_t expected) {
  return sys2(SYS_BLOCK_WAIT, (uint64_t)addr, expected);
}

static inline uint64_t sys_proc_create(void) { return sys0(SYS_PROC_CREATE); }
static inline uint64_t sys_thread_spawn(uint64_t pid, uint64_t entry,
                                        uint64_t stack_top, uint64_t arg) {
  return sys4(SYS_THREAD_SPAWN, pid, entry, stack_top, arg);
}
static inline uint64_t sys_proc_kill(uint64_t pid) {
  return sys1(SYS_PROC_KILL, pid);
}
static inline uint64_t sys_proc_reap(uint64_t pid) {
  return sys1(SYS_PROC_REAP, pid);
}

// ---------------------------------------------------------------------------
// Conveniences
// ---------------------------------------------------------------------------

// Debug-console output (SYS_DEBUG_WRITE — serial, until a video server
// exists).
static inline void print(const char *s) { sys_debug_write(s, strlen(s)); }

static inline void print_hex(uint64_t v) {
  char buf[17];
  for (int i = 15; i >= 0; i--) {
    uint64_t d = v & 0xF;
    buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  buf[16] = '\n';
  sys_debug_write(buf, 17);
}

// The image's load address == the base of the ublock the loader put it
// in, so this is what VM_SHARE wants. lld-link provides the
// pseudo-symbol per binary.
extern char __ImageBase;

#endif // sys_h_INCLUDED
