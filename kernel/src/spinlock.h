#ifndef spinlock_h_INCLUDED
#define spinlock_h_INCLUDED

#include <stdatomic.h>

struct spinlock {
  _Atomic bool locked;
};

#define SPINLOCK_INITIALIZER { .locked = false }

void spinlock_init(struct spinlock *lock);

// disables irqs when lock is secured
void spinlock_lock(struct spinlock *lock);

// enables irqs once lock is
void spinlock_unlock(struct spinlock *lock);

// Shootdown-servicing spinlock. Same shape as struct spinlock, but a
// waiter keeps servicing the in-flight TLB shootdown while it spins.
// Mandatory for any lock that can be held across as_flush by another
// CPU: the flush initiator waits IRQs-off for remote acks, and a CPU
// spinning IRQs-off on such a lock may be the very target it is waiting
// on (the IPI cannot be delivered). Zero-initialized == unlocked.
struct svclock {
  _Atomic bool locked;
};

#define SVCLOCK_INITIALIZER { .locked = false }

void svclock_lock(struct svclock *lock);
void svclock_unlock(struct svclock *lock);

#endif // spinlock_h_INCLUDED
