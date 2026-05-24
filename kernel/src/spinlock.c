#include "spinlock.h"

#include "irq.h"

static inline void spinlock_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#else
  atomic_signal_fence(memory_order_seq_cst);
#endif
}

void spinlock_init(struct spinlock *lock) {
  atomic_store_explicit(&lock->locked, false, memory_order_relaxed);
}

// Raw CAS attempt. No IRQ manipulation — only the public
// spinlock_lock/unlock pair touches the per-CPU IRQ depth.
static bool try_acquire(struct spinlock *lock) {
  bool expected = false;
  return atomic_compare_exchange_strong_explicit(
      &lock->locked, &expected, true,
      memory_order_acquire, memory_order_relaxed);
}

// Disable IRQs *before* the CAS loop: spinning with IRQs on while an
// IRQ handler on this CPU also wants the lock would deadlock. The
// irq_disable/enable pair nests via the per-CPU depth counter, so
// nested locks are fine.
void spinlock_lock(struct spinlock *lock) {
  irq_disable();
  for (;;) {
    if (try_acquire(lock)) {
      return;
    }
    while (atomic_load_explicit(&lock->locked, memory_order_relaxed)) {
      spinlock_relax();
    }
  }
}

void spinlock_unlock(struct spinlock *lock) {
  atomic_store_explicit(&lock->locked, false, memory_order_release);
  irq_enable();
}
