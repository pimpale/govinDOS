#ifndef pthread_h_INCLUDED
#define pthread_h_INCLUDED

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct __gdos_pthread;
typedef struct __gdos_pthread *pthread_t;
typedef unsigned pthread_key_t;
typedef _Atomic uint32_t pthread_once_t;
typedef _Atomic uint32_t pthread_spinlock_t;

typedef struct {
  size_t stack_size;
  size_t guard_size;
  void *stack_addr;
  int detach_state;
} pthread_attr_t;

typedef struct {
  int type;
  int pshared;
} pthread_mutexattr_t;

typedef struct {
  // Futex word: 0 free, 1 locked, 2 locked with (possible) waiters.
  // Uncontended lock and unlock make no syscalls.
  _Atomic uint32_t locked;
  _Atomic uint64_t owner;
  uint32_t recursion;
  uint32_t type;
} pthread_mutex_t;

typedef struct {
  _Atomic uint32_t sequence;
  // Last mutex waited with, recorded so broadcast can requeue waiters
  // onto it instead of stampeding them (futex-design.md §2).
  _Atomic uintptr_t mutex;
} pthread_cond_t;

typedef struct {
  int pshared;
  int clock;
} pthread_condattr_t;

typedef struct {
  _Atomic int32_t state;
} pthread_rwlock_t;

typedef struct {
  int pshared;
} pthread_rwlockattr_t;

typedef struct {
  _Atomic uint32_t count;
  _Atomic uint32_t generation;
  uint32_t trip_count;
} pthread_barrier_t;

typedef struct {
  int pshared;
} pthread_barrierattr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void *)-1)

#define PTHREAD_STACK_MIN 16384
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_KEYS_MAX 64
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

#define PTHREAD_MUTEX_INITIALIZER                                             \
  { 0, 0, 0, PTHREAD_MUTEX_NORMAL }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0 }
#define PTHREAD_ONCE_INIT 0

int pthread_create(pthread_t *restrict thread,
                   const pthread_attr_t *restrict attr,
                   void *(*start_routine)(void *), void *restrict arg);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
[[noreturn]] void pthread_exit(void *result);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstack(const pthread_attr_t *restrict attr,
                          void **restrict stack, size_t *restrict size);
int pthread_attr_setstack(pthread_attr_t *attr, void *stack, size_t size);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *restrict attr,
                              int *restrict type);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *restrict attr,
                                 int *restrict pshared);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared);
int pthread_mutex_init(pthread_mutex_t *restrict mutex,
                       const pthread_mutexattr_t *restrict attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *restrict mutex,
                            const struct timespec *restrict abstime);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_getpshared(const pthread_condattr_t *restrict attr,
                                int *restrict pshared);
int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared);
int pthread_condattr_getclock(const pthread_condattr_t *restrict attr,
                              clockid_t *restrict clock);
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock);
int pthread_cond_init(pthread_cond_t *restrict cond,
                      const pthread_condattr_t *restrict attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *restrict cond,
                      pthread_mutex_t *restrict mutex);
int pthread_cond_timedwait(pthread_cond_t *restrict cond,
                           pthread_mutex_t *restrict mutex,
                           const struct timespec *restrict abstime);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_once(pthread_once_t *once, void (*init_routine)(void));

int pthread_rwlock_init(pthread_rwlock_t *restrict lock,
                        const pthread_rwlockattr_t *restrict attr);
int pthread_rwlock_destroy(pthread_rwlock_t *lock);
int pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock);
int pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *lock);
int pthread_rwlock_unlock(pthread_rwlock_t *lock);
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *restrict attr,
                                  int *restrict pshared);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared);

int pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

int pthread_barrier_init(pthread_barrier_t *restrict barrier,
                         const pthread_barrierattr_t *restrict attr,
                         unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);
int pthread_barrierattr_init(pthread_barrierattr_t *attr);
int pthread_barrierattr_destroy(pthread_barrierattr_t *attr);
int pthread_barrierattr_getpshared(
    const pthread_barrierattr_t *restrict attr, int *restrict pshared);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);
int pthread_cancel(pthread_t thread);
void pthread_testcancel(void);

struct __pthread_cleanup {
  void (*routine)(void *);
  void *argument;
  struct __pthread_cleanup *previous;
};
void __pthread_cleanup_push(struct __pthread_cleanup *cleanup,
                            void (*routine)(void *), void *argument);
void __pthread_cleanup_pop(struct __pthread_cleanup *cleanup, int execute);

#define pthread_cleanup_push(routine, argument)                               \
  do {                                                                        \
    struct __pthread_cleanup __cleanup;                                       \
    __pthread_cleanup_push(&__cleanup, (routine), (argument));
#define pthread_cleanup_pop(execute)                                          \
    __pthread_cleanup_pop(&__cleanup, (execute));                             \
  } while (0)

#endif // pthread_h_INCLUDED
