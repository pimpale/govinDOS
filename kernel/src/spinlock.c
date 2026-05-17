#include "spinlock.h"

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

bool spinlock_try_lock(struct spinlock *lock) {
  bool expected = false;
  return atomic_compare_exchange_strong_explicit(
      &lock->locked, &expected, true,
      memory_order_acquire, memory_order_relaxed);
}

void spinlock_lock(struct spinlock *lock) {
  for (;;) {
    if (spinlock_try_lock(lock)) {
      return;
    }

    while (atomic_load_explicit(&lock->locked, memory_order_relaxed)) {
      spinlock_relax();
    }
  }
}

void spinlock_unlock(struct spinlock *lock) {
  atomic_store_explicit(&lock->locked, false, memory_order_release);
}
