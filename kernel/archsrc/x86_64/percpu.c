#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"

// Per-CPU addressing via IA32_TSC_AUX + RDPID.
//
// cpu_percpu_install writes a tagged CPU index into IA32_TSC_AUX on each
// CPU; cpu_percpu_try_get reads it back with RDPID and indexes
// g_cpu_state_table, making cpu_state_this O(1).
//
// Why this over the traditional GS-base approach: IA32_TSC_AUX is
// writable only via wrmsr (CPL 0), and RDPID only *reads* it — there is
// no unprivileged door like the GS selector reload that can clobber
// kernel state from ring 3, and no swapgs balancing discipline to get
// wrong on entry paths later (a SYSCALL-era entry stub can locate the
// kernel stack the same way: rdpid + table index). Ring 3 learning its
// own CPU index is harmless (that's how Linux implements vDSO getcpu).
// aarch64 gets the same interface for free via TPIDR_EL1.
//
// RDPID is required (Ice Lake / Zen 2 and later); older CPUs are out of
// scope by design. Under QEMU run with `-cpu max` — the default qemu64
// model doesn't advertise it.
//
// The tag distinguishes "installed" from the reset value (0) so early
// boot — before this CPU ran cpu_percpu_install — falls back cleanly to
// the hwid scan in cpu_state_this.

#define IA32_TSC_AUX 0xC0000103u

#define PERCPU_TAG      0xC0DE0000u
#define PERCPU_TAG_MASK 0xFFFF0000u
#define PERCPU_ID_MASK  0x0000FFFFu

static void wrmsr64(uint32_t msr, uint64_t val) {
  __asm__ volatile("wrmsr"
                   :
                   : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(subleaf));
}

// One-time sanity check so a missing-RDPID machine dies with a message
// instead of an opaque #UD. The flag is set *before* fatal's printing:
// its output path can reenter cpu_state_this -> here, and must not loop.
static void require_rdpid(void) {
  static bool checked = false;
  if (checked) {
    return;
  }
  checked = true;
  uint32_t a, b, c, d;
  cpuid(0, 0, &a, &b, &c, &d);
  if (a >= 7) {
    cpuid(7, 0, &a, &b, &c, &d);
    if (c & (1u << 22)) {
      return;
    }
  }
  fatal("percpu: CPU lacks RDPID (this kernel requires it; "
        "under QEMU use -cpu max)");
}

void cpu_percpu_install(struct cpu_state *cs) {
  require_rdpid();
  asserts(cs->logical_id <= PERCPU_ID_MASK,
          "percpu: logical id exceeds TSC_AUX tag space");
  wrmsr64(IA32_TSC_AUX, PERCPU_TAG | (uint32_t)cs->logical_id);
}

struct cpu_state *cpu_percpu_try_get(void) {
  require_rdpid();
  uint64_t v;
  __asm__ volatile("rdpid %0" : "=r"(v));
  uint32_t aux = (uint32_t)v;
  if ((aux & PERCPU_TAG_MASK) != PERCPU_TAG) {
    return nullptr; // reset value: this CPU hasn't installed yet
  }
  uint32_t id = aux & PERCPU_ID_MASK;
  if (id >= g_cpu_state_table_len) {
    return nullptr;
  }
  return &g_cpu_state_table[id];
}
