#include "pthread.h"

#include <errno.h>
#include <gdos/sys.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "libc_internal.h"
#include "string.h"

#define DEFAULT_STACK_SIZE (64u * 1024u)
#define DEFAULT_GUARD_SIZE 4096u

enum thread_lifecycle {
  THREAD_JOINABLE = 0,
  THREAD_JOINING = 1,
  THREAD_DETACHED = 2,
};

struct __gdos_pthread {
  // Must live in this thread object's private VM block: the kernel pins the
  // block and release-publishes completion after the thread is descheduled.
  _Atomic uint32_t complete;
  _Atomic uint32_t lifecycle;
  _Atomic uint32_t reap_queued;
  _Atomic uint32_t exit_started;
  uint64_t tid;
  void *(*start_routine)(void *);
  void *argument;
  void *result;
  uint64_t stack_base;
  uint64_t tls_base;
  bool owns_stack;
  struct __gdos_pthread *reap_next;
};

static _Thread_local struct __gdos_pthread *g_current_thread;
static _Thread_local struct __gdos_pthread g_implicit_thread;
static _Thread_local struct __pthread_cleanup *g_cleanup_stack;
static _Thread_local int g_cancel_state = PTHREAD_CANCEL_ENABLE;
static _Thread_local int g_cancel_type = PTHREAD_CANCEL_DEFERRED;

struct tsd_key {
  _Atomic uint32_t active;
  _Atomic uint32_t generation;
  void (*destructor)(void *);
};

static struct tsd_key g_tsd_keys[PTHREAD_KEYS_MAX];
static _Thread_local void *g_tsd_values[PTHREAD_KEYS_MAX];
static _Thread_local uint32_t g_tsd_generations[PTHREAD_KEYS_MAX];

struct reaper_event {
  _Atomic uint32_t sequence;
};

static _Atomic uint32_t g_reaper_state;
static struct reaper_event *g_reaper_event;
static atomic_flag g_reaper_lock = ATOMIC_FLAG_INIT;
static struct __gdos_pthread *g_reaper_list;

static void spin_lock(atomic_flag *lock) {
  while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
    sys_yield();
  }
}

static void spin_unlock(atomic_flag *lock) {
  atomic_flag_clear_explicit(lock, memory_order_release);
}

// Doorbells are block-scoped. Waking a waiter for another predicate in the
// same allocation is harmless because every pthread wait rechecks atomically;
// drain all bounded batches so the waiter interested in this transition is
// guaranteed to run as well.
static void wake_private_block(const volatile void *address) {
  uint64_t rc;
  do {
    rc = sys_block_doorbell((uint64_t)address);
    if (rc == SYSERR_AGAIN)
      sys_yield();
  } while (rc == SYSERR_AGAIN);
}

static int wait_u32(const volatile _Atomic uint32_t *address,
                    uint32_t expected) {
  return sys_block_wait(address, expected) == 0 ? 0 : EINVAL;
}

static uint64_t page_ceil(uint64_t value) {
  return (value + 4095) & ~(uint64_t)4095;
}

static void reclaim_thread(struct __gdos_pthread *thread) {
  if (thread->owns_stack) {
    sys_vm_free(thread->stack_base);
  }
  sys_vm_free(thread->tls_base);
  free(thread);
}

static void wait_for_completion(struct __gdos_pthread *thread) {
  while (atomic_load_explicit(&thread->complete, memory_order_acquire) !=
         GDOS_THREAD_COMPLETE) {
    uint64_t rc = sys_block_wait(&thread->complete, GDOS_THREAD_PENDING);
    if (sys_iserr(rc)) {
      sys_yield();
    }
  }
}

[[noreturn]] static void reaper_entry(uint64_t ignored) {
  (void)ignored;
  for (;;) {
    struct __gdos_pthread *victim = nullptr;
    uint32_t observed;

    spin_lock(&g_reaper_lock);
    struct __gdos_pthread **link = &g_reaper_list;
    while (*link != nullptr) {
      struct __gdos_pthread *candidate = *link;
      if (atomic_load_explicit(&candidate->exit_started,
                               memory_order_acquire) != 0 ||
          atomic_load_explicit(&candidate->complete,
                               memory_order_acquire) == GDOS_THREAD_COMPLETE) {
        *link = candidate->reap_next;
        victim = candidate;
        break;
      }
      link = &candidate->reap_next;
    }
    observed = atomic_load_explicit(&g_reaper_event->sequence,
                                    memory_order_relaxed);
    spin_unlock(&g_reaper_lock);

    if (victim != nullptr) {
      wait_for_completion(victim);
      reclaim_thread(victim);
      continue;
    }
    sys_block_wait(&g_reaper_event->sequence, observed);
  }
}

static int ensure_reaper(void) {
  uint32_t expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &g_reaper_state, &expected, 1, memory_order_acq_rel,
          memory_order_acquire)) {
    while ((expected = atomic_load_explicit(&g_reaper_state,
                                             memory_order_acquire)) == 1) {
      sys_yield();
    }
    return expected == 2 ? 0 : EAGAIN;
  }

  uint64_t event =
      sys_vm_alloc(4096, VM_PROT_READ | VM_PROT_WRITE);
  uint64_t stack =
      sys_vm_alloc(DEFAULT_STACK_SIZE, VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(event) || sys_iserr(stack)) {
    if (!sys_iserr(event))
      sys_vm_free(event);
    if (!sys_iserr(stack))
      sys_vm_free(stack);
    atomic_store_explicit(&g_reaper_state, 3, memory_order_release);
    return EAGAIN;
  }
  uint64_t stack_bytes = sys_vm_size(stack);
  if (sys_iserr(stack_bytes) ||
      sys_vm_protect(stack, DEFAULT_GUARD_SIZE, 0) != 0) {
    sys_vm_free(event);
    sys_vm_free(stack);
    atomic_store_explicit(&g_reaper_state, 3, memory_order_release);
    return EAGAIN;
  }
  uint64_t tls =
      __gdos_tls_create(stack, stack_bytes, DEFAULT_GUARD_SIZE);
  if (tls == 0) {
    sys_vm_free(event);
    sys_vm_free(stack);
    atomic_store_explicit(&g_reaper_state, 3, memory_order_release);
    return EAGAIN;
  }

  g_reaper_event = (void *)event;
  struct gdos_thread_start start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(start),
      .entry = (uint64_t)reaper_entry,
      .stack_pointer =
          stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
      .gs_base = tls,
  };
  uint64_t tid = sys_thread_spawn(sys_getpid(), &start);
  if (sys_iserr(tid)) {
    g_reaper_event = nullptr;
    sys_vm_free(tls);
    sys_vm_free(event);
    sys_vm_free(stack);
    atomic_store_explicit(&g_reaper_state, 3, memory_order_release);
    return EAGAIN;
  }
  // The reaper is process-lifetime infrastructure; its stack, TLS, and event
  // block are reclaimed with the process.
  atomic_store_explicit(&g_reaper_state, 2, memory_order_release);
  return 0;
}

static void notify_reaper(struct __gdos_pthread *thread) {
  if (atomic_exchange_explicit(&thread->reap_queued, 1,
                               memory_order_acq_rel) == 0) {
    spin_lock(&g_reaper_lock);
    thread->reap_next = g_reaper_list;
    g_reaper_list = thread;
    spin_unlock(&g_reaper_lock);
  }
  atomic_fetch_add_explicit(&g_reaper_event->sequence, 1,
                            memory_order_release);
  sys_block_doorbell((uint64_t)g_reaper_event);
}

static void run_tsd_destructors(void) {
  for (unsigned pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS; pass++) {
    bool called = false;
    for (unsigned key = 0; key < PTHREAD_KEYS_MAX; key++) {
      void *value = g_tsd_values[key];
      uint32_t generation = atomic_load_explicit(
          &g_tsd_keys[key].generation, memory_order_acquire);
      if (value == nullptr ||
          atomic_load_explicit(&g_tsd_keys[key].active,
                               memory_order_acquire) == 0 ||
          g_tsd_generations[key] != generation ||
          g_tsd_keys[key].destructor == nullptr) {
        continue;
      }
      g_tsd_values[key] = nullptr;
      called = true;
      g_tsd_keys[key].destructor(value);
    }
    if (!called)
      break;
  }
}

[[noreturn]] static void thread_entry(uint64_t raw_thread) {
  struct __gdos_pthread *thread = (void *)raw_thread;
  g_current_thread = thread;
  thread->tid = sys_gettid();
  pthread_exit(thread->start_routine(thread->argument));
}

pthread_t pthread_self(void) {
  if (g_current_thread == nullptr) {
    g_implicit_thread.tid = sys_gettid();
    g_current_thread = &g_implicit_thread;
  }
  return g_current_thread;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_create(pthread_t *restrict result,
                   const pthread_attr_t *restrict requested_attr,
                   void *(*start_routine)(void *), void *restrict argument) {
  if (result == nullptr || start_routine == nullptr)
    return EINVAL;

  pthread_attr_t defaults;
  if (requested_attr == nullptr) {
    pthread_attr_init(&defaults);
    requested_attr = &defaults;
  }
  if (requested_attr->detach_state == PTHREAD_CREATE_DETACHED) {
    int rc = ensure_reaper();
    if (rc != 0)
      return rc;
  }

  struct __gdos_pthread *thread = calloc(1, sizeof(*thread));
  if (thread == nullptr)
    return EAGAIN;

  uint64_t stack;
  uint64_t stack_bytes;
  uint64_t guard = page_ceil(requested_attr->guard_size);
  bool owns_stack = requested_attr->stack_addr == nullptr;
  if (owns_stack) {
    stack = sys_vm_alloc(requested_attr->stack_size,
                         VM_PROT_READ | VM_PROT_WRITE);
    if (sys_iserr(stack)) {
      free(thread);
      return EAGAIN;
    }
    stack_bytes = sys_vm_size(stack);
    if (sys_iserr(stack_bytes) || guard >= stack_bytes ||
        (guard != 0 && sys_vm_protect(stack, guard, 0) != 0)) {
      sys_vm_free(stack);
      free(thread);
      return EINVAL;
    }
  } else {
    stack = (uint64_t)requested_attr->stack_addr;
    stack_bytes = requested_attr->stack_size;
    guard = 0;
  }
  if (stack_bytes < PTHREAD_STACK_MIN ||
      (stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES) % 16 != 8) {
    if (owns_stack)
      sys_vm_free(stack);
    free(thread);
    return EINVAL;
  }

  uint64_t tls = __gdos_tls_create(stack, stack_bytes, guard);
  if (tls == 0) {
    if (owns_stack)
      sys_vm_free(stack);
    free(thread);
    return EAGAIN;
  }

  atomic_init(&thread->lifecycle,
              requested_attr->detach_state == PTHREAD_CREATE_DETACHED
                  ? THREAD_DETACHED
                  : THREAD_JOINABLE);
  thread->start_routine = start_routine;
  thread->argument = argument;
  thread->stack_base = stack;
  thread->tls_base = tls;
  thread->owns_stack = owns_stack;

  struct gdos_thread_start start = {
      .version = GDOS_THREAD_START_VERSION,
      .size = sizeof(start),
      .entry = (uint64_t)thread_entry,
      .argument = (uint64_t)thread,
      .stack_pointer =
          stack + stack_bytes - GDOS_THREAD_ENTRY_FRAME_BYTES,
      .gs_base = tls,
      .completion_event = (uint64_t)&thread->complete,
  };
  uint64_t tid = sys_thread_spawn(sys_getpid(), &start);
  if (sys_iserr(tid)) {
    sys_vm_free(tls);
    if (owns_stack)
      sys_vm_free(stack);
    free(thread);
    return EAGAIN;
  }
  thread->tid = tid;
  *result = thread;
  if (requested_attr->detach_state == PTHREAD_CREATE_DETACHED)
    notify_reaper(thread);
  return 0;
}

int pthread_join(pthread_t thread, void **result) {
  if (thread == nullptr)
    return ESRCH;
  if (thread == pthread_self())
    return EDEADLK;
  uint32_t expected = THREAD_JOINABLE;
  if (!atomic_compare_exchange_strong_explicit(
          &thread->lifecycle, &expected, THREAD_JOINING,
          memory_order_acq_rel, memory_order_acquire)) {
    return EINVAL;
  }
  wait_for_completion(thread);
  if (result != nullptr)
    *result = thread->result;
  reclaim_thread(thread);
  return 0;
}

int pthread_detach(pthread_t thread) {
  if (thread == nullptr)
    return ESRCH;
  int rc = ensure_reaper();
  if (rc != 0)
    return rc;
  uint32_t expected = THREAD_JOINABLE;
  if (!atomic_compare_exchange_strong_explicit(
          &thread->lifecycle, &expected, THREAD_DETACHED,
          memory_order_acq_rel, memory_order_acquire)) {
    return EINVAL;
  }
  notify_reaper(thread);
  return 0;
}

[[noreturn]] void pthread_exit(void *result) {
  struct __gdos_pthread *self = pthread_self();
  while (g_cleanup_stack != nullptr) {
    struct __pthread_cleanup *cleanup = g_cleanup_stack;
    g_cleanup_stack = cleanup->previous;
    cleanup->routine(cleanup->argument);
  }
  run_tsd_destructors();
  self->result = result;
  if (self != &g_implicit_thread) {
    atomic_store_explicit(&self->exit_started, 1, memory_order_release);
    if (atomic_load_explicit(&self->lifecycle, memory_order_acquire) ==
        THREAD_DETACHED) {
      notify_reaper(self);
    }
  }
  sys_thread_exit();
}

int pthread_attr_init(pthread_attr_t *attr) {
  if (attr == nullptr)
    return EINVAL;
  *attr = (pthread_attr_t){
      .stack_size = DEFAULT_STACK_SIZE,
      .guard_size = DEFAULT_GUARD_SIZE,
      .detach_state = PTHREAD_CREATE_JOINABLE,
  };
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
  return attr == nullptr ? EINVAL : 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state) {
  if (attr == nullptr || state == nullptr)
    return EINVAL;
  *state = attr->detach_state;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
  if (attr == nullptr ||
      (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED))
    return EINVAL;
  attr->detach_state = state;
  return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size) {
  if (attr == nullptr || size == nullptr)
    return EINVAL;
  *size = attr->guard_size;
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t size) {
  if (attr == nullptr)
    return EINVAL;
  attr->guard_size = size;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size) {
  if (attr == nullptr || size == nullptr)
    return EINVAL;
  *size = attr->stack_size;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
  if (attr == nullptr || size < PTHREAD_STACK_MIN)
    return EINVAL;
  attr->stack_size = size;
  attr->stack_addr = nullptr;
  return 0;
}

int pthread_attr_getstack(const pthread_attr_t *restrict attr,
                          void **restrict stack, size_t *restrict size) {
  if (attr == nullptr || stack == nullptr || size == nullptr)
    return EINVAL;
  *stack = attr->stack_addr;
  *size = attr->stack_size;
  return 0;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stack, size_t size) {
  if (attr == nullptr || stack == nullptr || size < PTHREAD_STACK_MIN ||
      ((uintptr_t)stack & 15) != 0 || (size & 15) != 0)
    return EINVAL;
  attr->stack_addr = stack;
  attr->stack_size = size;
  attr->guard_size = 0;
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
  if (attr == nullptr)
    return EINVAL;
  *attr = (pthread_mutexattr_t){.type = PTHREAD_MUTEX_NORMAL,
                                .pshared = PTHREAD_PROCESS_PRIVATE};
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
  return attr == nullptr ? EINVAL : 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *restrict attr,
                              int *restrict type) {
  if (attr == nullptr || type == nullptr)
    return EINVAL;
  *type = attr->type;
  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
  if (attr == nullptr || type < PTHREAD_MUTEX_NORMAL ||
      type > PTHREAD_MUTEX_ERRORCHECK)
    return EINVAL;
  attr->type = type;
  return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *restrict attr,
                                 int *restrict pshared) {
  if (attr == nullptr || pshared == nullptr)
    return EINVAL;
  *pshared = attr->pshared;
  return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared) {
  if (attr == nullptr || pshared != PTHREAD_PROCESS_PRIVATE)
    return pshared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
  attr->pshared = pshared;
  return 0;
}

int pthread_mutex_init(pthread_mutex_t *restrict mutex,
                       const pthread_mutexattr_t *restrict attr) {
  if (mutex == nullptr)
    return EINVAL;
  if (attr != nullptr && attr->pshared != PTHREAD_PROCESS_PRIVATE)
    return ENOTSUP;
  atomic_init(&mutex->locked, 0);
  atomic_init(&mutex->owner, 0);
  mutex->recursion = 0;
  mutex->type = attr == nullptr ? PTHREAD_MUTEX_NORMAL : attr->type;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  if (mutex == nullptr)
    return EINVAL;
  return atomic_load_explicit(&mutex->locked, memory_order_relaxed) ? EBUSY
                                                                   : 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  if (mutex == nullptr)
    return EINVAL;
  uint64_t tid = sys_gettid();
  if (atomic_load_explicit(&mutex->locked, memory_order_relaxed) &&
      atomic_load_explicit(&mutex->owner, memory_order_relaxed) == tid) {
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
      if (mutex->recursion == UINT32_MAX)
        return EAGAIN;
      mutex->recursion++;
      return 0;
    }
    return mutex->type == PTHREAD_MUTEX_ERRORCHECK ? EDEADLK : EBUSY;
  }
  uint32_t expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &mutex->locked, &expected, 1, memory_order_acquire,
          memory_order_relaxed))
    return EBUSY;
  atomic_store_explicit(&mutex->owner, tid, memory_order_relaxed);
  mutex->recursion = 1;
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  int rc;
  while ((rc = pthread_mutex_trylock(mutex)) == EBUSY) {
    if (wait_u32(&mutex->locked, 1) != 0)
      return EINVAL;
  }
  return rc;
}

int pthread_mutex_timedlock(pthread_mutex_t *restrict mutex,
                            const struct timespec *restrict abstime) {
  (void)mutex;
  (void)abstime;
  return ENOTSUP;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (mutex == nullptr)
    return EINVAL;
  uint64_t tid = sys_gettid();
  if (!atomic_load_explicit(&mutex->locked, memory_order_relaxed) ||
      atomic_load_explicit(&mutex->owner, memory_order_relaxed) != tid)
    return EPERM;
  if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->recursion > 1) {
    mutex->recursion--;
    return 0;
  }
  mutex->recursion = 0;
  atomic_store_explicit(&mutex->owner, 0, memory_order_relaxed);
  atomic_store_explicit(&mutex->locked, 0, memory_order_release);
  wake_private_block(&mutex->locked);
  return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
  if (attr == nullptr)
    return EINVAL;
  *attr = (pthread_condattr_t){.pshared = PTHREAD_PROCESS_PRIVATE,
                               .clock = CLOCK_REALTIME};
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
  return attr == nullptr ? EINVAL : 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *restrict attr,
                                int *restrict pshared) {
  if (attr == nullptr || pshared == nullptr)
    return EINVAL;
  *pshared = attr->pshared;
  return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared) {
  if (attr == nullptr || pshared != PTHREAD_PROCESS_PRIVATE)
    return pshared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
  return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *restrict attr,
                              clockid_t *restrict clock) {
  if (attr == nullptr || clock == nullptr)
    return EINVAL;
  *clock = attr->clock;
  return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) {
  if (attr == nullptr ||
      (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC))
    return EINVAL;
  attr->clock = clock;
  return 0;
}

int pthread_cond_init(pthread_cond_t *restrict cond,
                      const pthread_condattr_t *restrict attr) {
  if (cond == nullptr)
    return EINVAL;
  if (attr != nullptr && attr->pshared != PTHREAD_PROCESS_PRIVATE)
    return ENOTSUP;
  atomic_init(&cond->sequence, 0);
  return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
  return cond == nullptr ? EINVAL : 0;
}

int pthread_cond_wait(pthread_cond_t *restrict cond,
                      pthread_mutex_t *restrict mutex) {
  if (cond == nullptr || mutex == nullptr)
    return EINVAL;
  uint32_t sequence =
      atomic_load_explicit(&cond->sequence, memory_order_acquire);
  int rc = pthread_mutex_unlock(mutex);
  if (rc != 0)
    return rc;
  while (atomic_load_explicit(&cond->sequence, memory_order_acquire) ==
         sequence) {
    if (wait_u32(&cond->sequence, sequence) != 0)
      return EINVAL;
  }
  return pthread_mutex_lock(mutex);
}

int pthread_cond_timedwait(pthread_cond_t *restrict cond,
                           pthread_mutex_t *restrict mutex,
                           const struct timespec *restrict abstime) {
  (void)cond;
  (void)mutex;
  (void)abstime;
  return ENOTSUP;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  if (cond == nullptr)
    return EINVAL;
  atomic_fetch_add_explicit(&cond->sequence, 1, memory_order_release);
  wake_private_block(&cond->sequence);
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  return pthread_cond_signal(cond);
}

int pthread_once(pthread_once_t *once, void (*init_routine)(void)) {
  if (once == nullptr || init_routine == nullptr)
    return EINVAL;
  uint32_t state = atomic_load_explicit(once, memory_order_acquire);
  if (state == 2)
    return 0;
  uint32_t expected = 0;
  if (atomic_compare_exchange_strong_explicit(
          once, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
    init_routine();
    atomic_store_explicit(once, 2, memory_order_release);
    wake_private_block(once);
    return 0;
  }
  while (atomic_load_explicit(once, memory_order_acquire) != 2) {
    uint32_t observed = atomic_load_explicit(once, memory_order_relaxed);
    if (wait_u32(once, observed) != 0)
      return EINVAL;
  }
  return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *restrict lock,
                        const pthread_rwlockattr_t *restrict attr) {
  if (lock == nullptr)
    return EINVAL;
  if (attr != nullptr && attr->pshared != PTHREAD_PROCESS_PRIVATE)
    return ENOTSUP;
  atomic_init(&lock->state, 0);
  return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock) {
  if (lock == nullptr)
    return EINVAL;
  return atomic_load(&lock->state) == 0 ? 0 : EBUSY;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock) {
  if (lock == nullptr)
    return EINVAL;
  int32_t state = atomic_load_explicit(&lock->state, memory_order_relaxed);
  do {
    if (state < 0 || state == INT32_MAX)
      return EBUSY;
  } while (!atomic_compare_exchange_weak_explicit(
      &lock->state, &state, state + 1, memory_order_acquire,
      memory_order_relaxed));
  return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
  int rc;
  while ((rc = pthread_rwlock_tryrdlock(lock)) == EBUSY) {
    uint32_t observed = (uint32_t)atomic_load_explicit(
        &lock->state, memory_order_relaxed);
    if (wait_u32((volatile _Atomic uint32_t *)&lock->state, observed) != 0)
      return EINVAL;
  }
  return rc;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
  if (lock == nullptr)
    return EINVAL;
  int32_t expected = 0;
  return atomic_compare_exchange_strong_explicit(
             &lock->state, &expected, -1, memory_order_acquire,
             memory_order_relaxed)
             ? 0
             : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
  int rc;
  while ((rc = pthread_rwlock_trywrlock(lock)) == EBUSY) {
    uint32_t observed = (uint32_t)atomic_load_explicit(
        &lock->state, memory_order_relaxed);
    if (wait_u32((volatile _Atomic uint32_t *)&lock->state, observed) != 0)
      return EINVAL;
  }
  return rc;
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock) {
  if (lock == nullptr)
    return EINVAL;
  int32_t state = atomic_load_explicit(&lock->state, memory_order_relaxed);
  if (state == -1) {
    atomic_store_explicit(&lock->state, 0, memory_order_release);
    wake_private_block(&lock->state);
    return 0;
  }
  if (state <= 0)
    return EPERM;
  atomic_fetch_sub_explicit(&lock->state, 1, memory_order_release);
  wake_private_block(&lock->state);
  return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attr) {
  if (attr == nullptr)
    return EINVAL;
  attr->pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr) {
  return attr == nullptr ? EINVAL : 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *restrict attr,
                                  int *restrict pshared) {
  if (attr == nullptr || pshared == nullptr)
    return EINVAL;
  *pshared = attr->pshared;
  return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared) {
  if (attr == nullptr || pshared != PTHREAD_PROCESS_PRIVATE)
    return pshared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
  return 0;
}

int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
  if (lock == nullptr)
    return EINVAL;
  if (pshared != PTHREAD_PROCESS_PRIVATE)
    return pshared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
  atomic_init(lock, 0);
  return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
  return lock == nullptr ? EINVAL : 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
  if (lock == nullptr)
    return EINVAL;
  uint32_t expected = 0;
  return atomic_compare_exchange_strong_explicit(
             lock, &expected, 1, memory_order_acquire, memory_order_relaxed)
             ? 0
             : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
  int rc;
  while ((rc = pthread_spin_trylock(lock)) == EBUSY)
    __asm__ volatile("pause");
  return rc;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
  if (lock == nullptr)
    return EINVAL;
  atomic_store_explicit(lock, 0, memory_order_release);
  return 0;
}

int pthread_barrier_init(pthread_barrier_t *restrict barrier,
                         const pthread_barrierattr_t *restrict attr,
                         unsigned count) {
  if (barrier == nullptr || count == 0)
    return EINVAL;
  if (attr != nullptr && attr->pshared != PTHREAD_PROCESS_PRIVATE)
    return ENOTSUP;
  atomic_init(&barrier->count, 0);
  atomic_init(&barrier->generation, 0);
  barrier->trip_count = count;
  return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
  if (barrier == nullptr)
    return EINVAL;
  return atomic_load(&barrier->count) == 0 ? 0 : EBUSY;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
  if (barrier == nullptr || barrier->trip_count == 0)
    return EINVAL;
  uint32_t generation =
      atomic_load_explicit(&barrier->generation, memory_order_acquire);
  uint32_t arrived = atomic_fetch_add_explicit(&barrier->count, 1,
                                                memory_order_acq_rel) +
                     1;
  if (arrived == barrier->trip_count) {
    atomic_store_explicit(&barrier->count, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&barrier->generation, 1, memory_order_release);
    wake_private_block(&barrier->generation);
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }
  while (atomic_load_explicit(&barrier->generation, memory_order_acquire) ==
         generation) {
    if (wait_u32(&barrier->generation, generation) != 0)
      return EINVAL;
  }
  return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t *attr) {
  if (attr == nullptr)
    return EINVAL;
  attr->pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attr) {
  return attr == nullptr ? EINVAL : 0;
}

int pthread_barrierattr_getpshared(
    const pthread_barrierattr_t *restrict attr, int *restrict pshared) {
  if (attr == nullptr || pshared == nullptr)
    return EINVAL;
  *pshared = attr->pshared;
  return 0;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared) {
  if (attr == nullptr || pshared != PTHREAD_PROCESS_PRIVATE)
    return pshared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
  return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  if (key == nullptr)
    return EINVAL;
  for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &g_tsd_keys[i].active, &expected, 1, memory_order_acq_rel,
            memory_order_relaxed)) {
      g_tsd_keys[i].destructor = destructor;
      atomic_fetch_add_explicit(&g_tsd_keys[i].generation, 1,
                                memory_order_release);
      *key = i;
      return 0;
    }
  }
  return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX ||
      atomic_exchange_explicit(&g_tsd_keys[key].active, 0,
                               memory_order_acq_rel) == 0)
    return EINVAL;
  g_tsd_keys[key].destructor = nullptr;
  atomic_fetch_add_explicit(&g_tsd_keys[key].generation, 1,
                            memory_order_release);
  return 0;
}

void *pthread_getspecific(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX ||
      atomic_load_explicit(&g_tsd_keys[key].active, memory_order_acquire) == 0)
    return nullptr;
  uint32_t generation = atomic_load_explicit(&g_tsd_keys[key].generation,
                                              memory_order_acquire);
  return g_tsd_generations[key] == generation ? g_tsd_values[key] : nullptr;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key >= PTHREAD_KEYS_MAX ||
      atomic_load_explicit(&g_tsd_keys[key].active, memory_order_acquire) == 0)
    return EINVAL;
  g_tsd_values[key] = (void *)value;
  g_tsd_generations[key] = atomic_load_explicit(
      &g_tsd_keys[key].generation, memory_order_acquire);
  return 0;
}

int pthread_setcancelstate(int state, int *oldstate) {
  if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
    return EINVAL;
  if (oldstate != nullptr)
    *oldstate = g_cancel_state;
  g_cancel_state = state;
  return 0;
}

int pthread_setcanceltype(int type, int *oldtype) {
  if (type != PTHREAD_CANCEL_DEFERRED &&
      type != PTHREAD_CANCEL_ASYNCHRONOUS)
    return EINVAL;
  if (oldtype != nullptr)
    *oldtype = g_cancel_type;
  g_cancel_type = type;
  return 0;
}

int pthread_cancel(pthread_t thread) {
  (void)thread;
  return ENOTSUP;
}

void pthread_testcancel(void) {}

void __pthread_cleanup_push(struct __pthread_cleanup *cleanup,
                            void (*routine)(void *), void *argument) {
  cleanup->routine = routine;
  cleanup->argument = argument;
  cleanup->previous = g_cleanup_stack;
  g_cleanup_stack = cleanup;
}

void __pthread_cleanup_pop(struct __pthread_cleanup *cleanup, int execute) {
  if (g_cleanup_stack == cleanup)
    g_cleanup_stack = cleanup->previous;
  if (execute)
    cleanup->routine(cleanup->argument);
}
