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
#include "thread.h"
#include "trap_frame.h"

#define INT_NMI 0x02
#define INT_DOUBLE_FAULT 0x08
#define INT_GENERAL_PROTECTION 0x0D
#define INT_PAGE_FAULT 0x0E
#define INT_MACHINE_CHECK 0x12

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

// The register snapshot layout (struct trap_frame, trap_frame.h) must stay
// in sync with the push order in interrupts.asm. The asm passes rcx =
// pointer to the frame base; Win64 large-struct-byval had the same register
// contents, but the pointer signature makes the aliasing explicit — writes
// through `tf` (e.g. the syscall result into tf->rax) land in the actual
// on-stack frame that the asm restore path pops.
// Death checkpoint on the way back to ring 3: an interrupt is a killed
// thread's "next kernel entry". The trap frame was pushed by the ISR
// stub on the per-CPU stack; save it into the TCB (it is the thread's
// complete state) and let the dispatch cull free the TCB.
static void cull_if_killed(struct trap_frame *tf) {
  if ((tf->cs & 3) != 3) {
    return;
  }
  struct thread *curr = thread_current();
  if (curr != nullptr && curr->proc->state == PROC_DEAD) {
    uthread_park_exit(); // never returns
  }
}

uint64_t interrupt_handler(struct trap_frame *tf, uint64_t vector,
                           uint64_t error, uint64_t rip) {
  // Bump the per-CPU IRQ depth without touching IF (hardware already
  // cleared it on entry). This way, any spinlock_lock/unlock inside the
  // handler nests above depth=1 and the final unlock won't sti
  // mid-handler. iretq restores IF from the saved frame.
  irq_enter();
  switch (vector) {
  case VECTOR_TLB_SHOOTDOWN:
    paging_handle_tlb_shootdown();
    cull_if_killed(tf);
    irq_exit();
    return 0;
  case VECTOR_RESCHED:
    // Wake-up only. The HLT'd scheduler loop resumes after the IRET,
    // sees the new queue entry on its next iteration, and dispatches it.
    x86_lapic_eoi();
    cull_if_killed(tf);
    irq_exit();
    return 0;
  case VECTOR_PREEMPT:
    // Quantum expiry: an involuntary SYS_YIELD. From ring 3 the trap
    // frame on this stack is the thread's complete GPR state (the FPU
    // half is captured by the frame save), so park it requeued; the
    // dispatch that picks it back up arms a fresh quantum. In kernel
    // mode the only IF=1 context is the scheduler loop's own
    // idle/dispatch windows — a stale shot there is spurious, and that
    // loop reschedules on its own.
    x86_lapic_eoi();
    if ((tf->cs & 3) == 3) {
      cull_if_killed(tf); // preempted thread of a dead process exits here
      arch_uthread_save_frame(thread_current(), tf);
      uthread_park_yield(); // never returns
    }
    irq_exit();
    return 0;
  default:
    break;
  }

  uint64_t cr2 = (vector == INT_PAGE_FAULT) ? read_cr2() : 0;

  // A fault raised in ring 3 kills the faulting thread, not the kernel.
  // This is also the designed failure mode for revoked shared memory: the
  // owner freed (or died with) a block a sharer still had pointers into,
  // and the sharer's next touch lands here.
  if ((tf->cs & 3) == 3 &&
      (vector == INT_PAGE_FAULT || vector == INT_GENERAL_PROTECTION)) {
    struct thread *curr = thread_current();
    printf("user %s: killing pid=%llu tid=%llu rip=%016llX cr2=%016llX\n",
           exception_name(vector), curr->proc->pid, curr->tid, rip, cr2);
    uthread_park_exit(); // never returns; the scheduler loop reaps the TCB
  }

  const char *what = exception_name(vector);
  if (vector == INT_PAGE_FAULT && looks_like_stack_guard(cr2)) {
    what = "kernel stack overflow (guard page hit)";
  }
  // Returning 0 here would IRET back to the same RIP and re-fault forever.
  // Every unhandled vector is fatal until we grow real handlers. No
  // irq_exit before fault_panic — we're never coming back.
  fault_panic(what, vector, error, rip, cr2);
}
