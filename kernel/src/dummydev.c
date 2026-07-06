#include "dummydev.h"

#include <stdatomic.h>
#include <stdint.h>

#include "debug.h"
#include "scheduler.h"
#include "spinlock.h"
#include "thread.h"

// State: a queue of blocked readers plus a single sleeping-producer slot.
// The producer only runs (and burns a fake-latency delay) when there is a
// reader to serve; otherwise it blocks itself.
//
// Wake-up protocol (both directions) is the check-under-lock pattern:
// whoever wants to sleep publishes that intent under `lock` before
// blocking, and whoever wakes reads-and-clears it under the same lock.
// thread_unblock's on_cpu spin closes the "published but not yet
// descheduled" window. NOTE: the sleeper must go from publishing to
// blocking without yielding in between — revisit if kernel-side
// preemption ever lands.
static struct spinlock g_lock;
static list_thread_ptr *g_readers;
static struct thread *g_producer_sleeping;

static void dummydev_producer(void *arg) {
  (void)arg;
  uint64_t seq = 0;
  while (true) {
    spinlock_lock(&g_lock);
    if (list_thread_ptr_len(g_readers) == 0) {
      g_producer_sleeping = thread_current();
      spinlock_unlock(&g_lock);
      thread_block();
      continue;
    }
    spinlock_unlock(&g_lock);

    // Simulate device latency before producing.
    for (int i = 0; i < 200; i++) {
      yield();
    }

    spinlock_lock(&g_lock);
    struct thread *reader = nullptr;
    if (list_thread_ptr_len(g_readers) > 0) {
      list_thread_ptr_pop_front(g_readers, &reader);
    }
    spinlock_unlock(&g_lock);

    if (reader != nullptr) {
      thread_deliver_wait_result(reader, 0xD00D000000000000ull | seq++);
      thread_unblock(reader);
    }
  }
}

void dummydev_init(void) {
  spinlock_init(&g_lock);
  list_thread_ptr_new(&g_readers);
  g_producer_sleeping = nullptr;
  kthread_spawn(dummydev_producer, nullptr);
}

// Enqueue `t` as a reader and wake the producer if it is asleep. Shared
// by both entry points; returns with no locks held.
static void enqueue_reader(struct thread *t) {
  spinlock_lock(&g_lock);
  list_thread_ptr_push_back(g_readers, &t);
  struct thread *producer = g_producer_sleeping;
  g_producer_sleeping = nullptr;
  spinlock_unlock(&g_lock);

  if (producer != nullptr) {
    thread_unblock(producer);
  }
}

[[noreturn]] void dummydev_read_user(struct thread *curr) {
  enqueue_reader(curr);
  uthread_park_blocked();
}

uint64_t dummydev_read_kthread(void) {
  struct thread *curr = thread_current();
  enqueue_reader(curr);
  thread_block();
  return curr->wait_result;
}
