#include "xstate.h"

#include <stdbool.h>
#include <stdint.h>

#include "debug.h"
#include "stdlib/stdio.h"
#include "stdlib/string.h"

#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)

#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_OSXSAVE (1ull << 18)

#define CPUID_1_XSAVE (1u << 26)
#define CPUID_1_AVX (1u << 28)
#define CPUID_7_AVX512F (1u << 16)

#define XSTATE_X87 (1ull << 0)
#define XSTATE_SSE (1ull << 1)
#define XSTATE_AVX (1ull << 2)
#define XSTATE_OPMASK (1ull << 5)
#define XSTATE_ZMM_HI256 (1ull << 6)
#define XSTATE_HI16_ZMM (1ull << 7)

#define XSAVE_LEGACY_BYTES 512u
#define XSAVE_HEADER_BYTES 64u
#define XSAVE_MIN_BYTES (XSAVE_LEGACY_BYTES + XSAVE_HEADER_BYTES)

struct cpuid_result {
  uint32_t eax;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
};

static uint64_t g_xstate_mask;
static size_t g_xstate_bytes;
static bool g_xstate_ready;

static struct cpuid_result cpuid(uint32_t leaf, uint32_t subleaf) {
  struct cpuid_result r;
  __asm__ volatile("cpuid"
                   : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                   : "a"(leaf), "c"(subleaf));
  return r;
}

static uint64_t xgetbv0(void) {
  uint32_t lo;
  uint32_t hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return ((uint64_t)hi << 32) | lo;
}

static void xsetbv0(uint64_t value) {
  __asm__ volatile("xsetbv"
                   :
                   : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
                     "c"(0));
}

static uint64_t read_cr0(void) {
  uint64_t value;
  __asm__ volatile("mov %%cr0, %0" : "=r"(value));
  return value;
}

static void write_cr0(uint64_t value) {
  __asm__ volatile("mov %0, %%cr0" : : "r"(value));
}

static uint64_t read_cr4(void) {
  uint64_t value;
  __asm__ volatile("mov %%cr4, %0" : "=r"(value));
  return value;
}

static void write_cr4(uint64_t value) {
  __asm__ volatile("mov %0, %%cr4" : : "r"(value));
}

static size_t standard_area_size(uint64_t mask) {
  size_t bytes = XSAVE_MIN_BYTES;
  for (uint32_t component = 2; component < 64; component++) {
    if ((mask & (1ull << component)) == 0)
      continue;
    struct cpuid_result r = cpuid(0xD, component);
    asserts(r.eax != 0, "xstate: selected component has no save area");
    uint64_t end = (uint64_t)r.ebx + r.eax;
    asserts(end <= SIZE_MAX, "xstate: save area size overflow");
    if ((size_t)end > bytes)
      bytes = (size_t)end;
  }
  return bytes;
}

void x86_xstate_global_init(void) {
  asserts(!g_xstate_ready, "xstate: global init twice");
  struct cpuid_result max = cpuid(0, 0);
  asserts(max.eax >= 0xD, "xstate: CPUID leaf 0xD missing");

  struct cpuid_result features = cpuid(1, 0);
  asserts((features.ecx & CPUID_1_XSAVE) != 0,
          "xstate: XSAVE not supported");

  struct cpuid_result d0 = cpuid(0xD, 0);
  uint64_t supported = ((uint64_t)d0.edx << 32) | d0.eax;
  asserts((supported & (XSTATE_X87 | XSTATE_SSE)) ==
              (XSTATE_X87 | XSTATE_SSE),
          "xstate: x87/SSE state missing");

  uint64_t mask = XSTATE_X87 | XSTATE_SSE;
  if ((features.ecx & CPUID_1_AVX) != 0 &&
      (supported & XSTATE_AVX) != 0) {
    mask |= XSTATE_AVX;
  }

  if (max.eax >= 7 && (mask & XSTATE_AVX) != 0) {
    struct cpuid_result leaf7 = cpuid(7, 0);
    uint64_t avx512_state = XSTATE_OPMASK | XSTATE_ZMM_HI256 | XSTATE_HI16_ZMM;
    if ((leaf7.ebx & CPUID_7_AVX512F) != 0 &&
        (supported & avx512_state) == avx512_state) {
      mask |= avx512_state;
    }
  }

  g_xstate_mask = mask;
  g_xstate_bytes = standard_area_size(mask);
  g_xstate_ready = true;
  printf("xstate: mask=%016llX bytes=%llu\n", g_xstate_mask,
         (uint64_t)g_xstate_bytes);
}

void x86_xstate_cpu_init(void) {
  asserts(g_xstate_ready, "xstate: CPU init before global init");
  struct cpuid_result features = cpuid(1, 0);
  asserts((features.ecx & CPUID_1_XSAVE) != 0,
          "xstate: CPU lacks XSAVE");
  struct cpuid_result d0 = cpuid(0xD, 0);
  uint64_t supported = ((uint64_t)d0.edx << 32) | d0.eax;
  asserts((supported & g_xstate_mask) == g_xstate_mask,
          "xstate: heterogeneous CPU feature set");

  uint64_t cr0 = read_cr0();
  cr0 |= CR0_MP;
  cr0 &= ~(CR0_EM | CR0_TS);
  write_cr0(cr0);

  uint64_t cr4 = read_cr4();
  cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE;
  write_cr4(cr4);
  xsetbv0(g_xstate_mask);
  asserts(xgetbv0() == g_xstate_mask, "xstate: XCR0 did not latch");

  // CPUID.0D.0:EBX reports the size for the now-enabled XCR0 mask. It must
  // fit the standard-format size computed component-by-component on the BSP.
  d0 = cpuid(0xD, 0);
  asserts(d0.ebx <= g_xstate_bytes, "xstate: CPU save area larger than BSP");
}

size_t x86_xstate_area_size(void) {
  asserts(g_xstate_ready, "xstate: size before global init");
  return g_xstate_bytes;
}

uint64_t x86_xstate_mask(void) {
  asserts(g_xstate_ready, "xstate: mask before global init");
  return g_xstate_mask;
}

void x86_xstate_area_init(void *area) {
  asserts(((uintptr_t)area & 63) == 0, "xstate: unaligned save area");
  memset(area, 0, g_xstate_bytes);

  // Mark x87 and SSE present with their architectural reset controls. All
  // other enabled components have clear XSTATE_BV bits, so XRSTOR installs
  // their architectural initial state for a fresh thread.
  *(uint16_t *)((uint8_t *)area + 0) = 0x037F;
  *(uint32_t *)((uint8_t *)area + 24) = 0x1F80;
  *(uint64_t *)((uint8_t *)area + XSAVE_LEGACY_BYTES) =
      XSTATE_X87 | XSTATE_SSE;
}

void x86_xstate_save(void *area) {
  asserts(((uintptr_t)area & 63) == 0, "xstate: unaligned save area");
  __asm__ volatile("xsave64 (%0)"
                   :
                   : "r"(area), "a"((uint32_t)g_xstate_mask),
                     "d"((uint32_t)(g_xstate_mask >> 32))
                   : "memory");
}

void x86_xstate_restore(const void *area) {
  asserts(((uintptr_t)area & 63) == 0, "xstate: unaligned restore area");
  __asm__ volatile("xrstor64 (%0)"
                   :
                   : "r"(area), "a"((uint32_t)g_xstate_mask),
                     "d"((uint32_t)(g_xstate_mask >> 32))
                   : "memory");
}
