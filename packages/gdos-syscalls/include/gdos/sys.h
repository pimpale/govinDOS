#ifndef gdos_sys_INCLUDED

#include <stdint.h>

#include <gdosabi/syscall.h>
#include <gdosabi/kring_cap.h>
#include <gdosabi/thread.h>

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
  return sys4(nr, a0, a1, 0, 0);
}
static inline uint64_t sys1(uint64_t nr, uint64_t a0) {
  return sys4(nr, a0, 0, 0, 0);
}
static inline uint64_t sys0(uint64_t nr) { return sys4(nr, 0, 0, 0, 0); }

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

[[noreturn]] static inline void sys_thread_exit(void) {
  sys0(SYS_THREAD_EXIT);
  __builtin_unreachable();
}

[[noreturn]] static inline void sys_proc_exit(uint64_t status) {
  sys1(SYS_PROC_EXIT, status);
  __builtin_unreachable();
}

static inline void sys_yield(void) { sys0(SYS_YIELD); }
static inline uint64_t sys_getpid(void) { return sys0(SYS_GETPID); }
static inline uint64_t sys_gettid(void) { return sys0(SYS_GETTID); }

// Blocks are the allocation unit (power-of-two pages); returns the base.
static inline uint64_t sys_vm_alloc(uint64_t len, uint64_t prot) {
  return sys2(SYS_VM_ALLOC, len, prot);
}
// base alone names the block — blocks are the unit, no length needed.
static inline uint64_t sys_vm_free(uint64_t base) {
  return sys1(SYS_VM_FREE, base);
}
// Returns the usable, rounded byte capacity of an exact owned block base.
static inline uint64_t sys_vm_size(uint64_t base) {
  return sys1(SYS_VM_SIZE, base);
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
static inline uint64_t sys_vm_map_device(const struct cap_token *token,
                                         uint64_t flags) {
  return sys3(SYS_VM_MAP_DEVICE, (uint64_t)token, CAP_TOKEN_SIZE, flags);
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
static inline uint64_t
sys_thread_spawn(uint64_t pid, const struct gdos_thread_start *start) {
  return sys3(SYS_THREAD_SPAWN, pid, (uint64_t)start, sizeof(*start));
}
static inline uint64_t sys_thread_bases_set(uint64_t fs_base,
                                            uint64_t gs_base) {
  return sys2(SYS_THREAD_BASES_SET, fs_base, gs_base);
}
static inline uint64_t sys_proc_kill(uint64_t pid) {
  return sys1(SYS_PROC_KILL, pid);
}
static inline uint64_t sys_proc_reap(uint64_t pid) {
  return sys1(SYS_PROC_REAP, pid);
}

#endif // gdos_sys_INCLUDED
