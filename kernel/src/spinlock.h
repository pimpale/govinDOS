#ifndef spinlock_h_INCLUDED
#define spinlock_h_INCLUDED

#include <stdatomic.h>

struct spinlock {
  _Atomic bool locked;
};

#define SPINLOCK_INITIALIZER { .locked = false }

void spinlock_init(struct spinlock *lock);
bool spinlock_try_lock(struct spinlock *lock);
void spinlock_lock(struct spinlock *lock);
void spinlock_unlock(struct spinlock *lock);

#endif // spinlock_h_INCLUDED
