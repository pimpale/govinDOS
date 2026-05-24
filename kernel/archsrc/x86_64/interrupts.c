#include "interrupts.h"

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"
#include "irq.h"
#include "lapic.h"
#include "paging.h"
#include "panic.h"
#include "scheduler.h"
#include "stdlib/stdio.h"

#define INT_NMI 0x02
#define INT_DOUBLE_FAULT 0x08
#define INT_GENERAL_PROTECTION 0x0D
#define INT_PAGE_FAULT 0x0E
#define INT_MACHINE_CHECK 0x12
#define INT_SYSCALL 0x80

static inline uint64_t read_cr2(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr2, %0" : "=r"(v));
  return v;
}

// A kernel stack guard is a 4 KiB hole at the bottom of an otherwise-mapped
// stack: as_flag(g_as_kernel, base, base + PAGE_SIZE, 0). Any fault address
// that is itself not present in g_as_kernel but whose next page *is* present
// has the shape of one of those guards. Cheap and conservative — false
// positives are limited to the literal page just below any mapped region.
static bool looks_like_stack_guard(uint64_t cr2) {
  paging_flags_t f = 0;
  bool present = false;
  as_getinfo(g_as_kernel, cr2, &f, &present);
  if (present) {
    return false;
  }
  uint64_t above = (cr2 & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;
  as_getinfo(g_as_kernel, above, &f, &present);
  return present;
}

[[noreturn]] static void fault_panic(const char *what,
                                     uint64_t vector,
                                     uint64_t error,
                                     uint64_t rip,
                                     uint64_t cr2) {
  printf("\n*** kernel panic: %s ***\n", what);
  printf("  cpu=%llu vector=%llu error=%016llX\n",
         cpu_state_whoami(), vector, error);
  printf("  rip=%016llX cr2=%016llX\n", rip, cr2);
  panic();
}

static const char *exception_name(uint64_t vector) {
  switch (vector) {
  case INT_NMI:                return "non-maskable interrupt";
  case INT_DOUBLE_FAULT:       return "double fault";
  case INT_GENERAL_PROTECTION: return "general protection fault";
  case INT_PAGE_FAULT:         return "page fault";
  case INT_MACHINE_CHECK:      return "machine check";
  default:                     return "unhandled exception";
  }
}

// Register snapshot pushed by the asm ISR stub before calling in. Layout
// must stay in sync with the push order in interrupts.asm. Named distinctly
// from the global per-CPU `struct cpu_state` (cpu_state.h) so the two don't
// collide if both headers are ever pulled into the same TU.
struct [[gnu::packed]] isr_frame {
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t r9;
  uint64_t r8;
  uint64_t rdx;
};

uint64_t interrupt_handler(struct isr_frame regs, uint64_t vector, uint64_t error, uint64_t rip) {
  (void)regs;
  // Bump the per-CPU IRQ depth without touching IF (hardware already
  // cleared it on entry). This way, any spinlock_lock/unlock inside the
  // handler nests above depth=1 and the final unlock won't sti
  // mid-handler. iretq restores IF from the saved frame.
  irq_enter();
  switch (vector) {
  case VECTOR_TLB_SHOOTDOWN:
    paging_handle_tlb_shootdown();
    irq_exit();
    return 0;
  case VECTOR_RESCHED:
    // Wake-up only. The HLT'd scheduler loop resumes after the IRET,
    // sees the new queue entry on its next iteration, and dispatches it.
    x86_lapic_eoi();
    irq_exit();
    return 0;
  default:
    break;
  }

  uint64_t cr2 = (vector == INT_PAGE_FAULT) ? read_cr2() : 0;
  const char *what = exception_name(vector);
  if (vector == INT_PAGE_FAULT && looks_like_stack_guard(cr2)) {
    what = "kernel stack overflow (guard page hit)";
  }
  // Returning 0 here would IRET back to the same RIP and re-fault forever.
  // Every unhandled vector is fatal until we grow real handlers. No
  // irq_exit before fault_panic — we're never coming back.
  fault_panic(what, vector, error, rip, cr2);
}
