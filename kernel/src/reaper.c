#include "reaper.h"

#include <stdatomic.h>

#include "cpu_state.h"
#include "debug.h"
#include "paging.h"
#include "ring.h"
#include "scheduler.h"
#include "spinlock.h"
#include "stacks.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "thread.h"
#include "umem.h"

// Dead list + sleeping-reaper slot, the same check-under-lock wake
// protocol as dummydev/ring workers.
static struct spinlock g_lock;
static list_thread_ptr *g_dead;
static struct thread *g_reaper_sleeping;

void reaper_submit(struct thread *t) {
  spinlock_lock(&g_lock);
  list_thread_ptr_push_back(g_dead, &t);
  struct thread *reaper = g_reaper_sleeping;
  g_reaper_sleeping = nullptr;
  spinlock_unlock(&g_lock);
  if (reaper != nullptr) {
    thread_unblock(reaper);
  }
}

void process_destroy(struct process *p) {
  asserts(p != g_kernel_process, "reaper: kernel process cannot die");
  asserts(atomic_load(&p->nthreads) == 0,
          "reaper: destroying a process with live threads");

  // Drain: no CPU may still have this AS current when we free its page
  // tree. All the process's threads are dead, so each CPU leaves the AS
  // at its next dispatch (arch_thread_install switches) or idle iteration
  // (the scheduler switches to g_as_kernel before HLT). Bounded wait.
  for (size_t i = 0; i < g_cpu_state_table_len; i++) {
    while (g_cpu_state_table[i].current_as == p->as) {
      yield();
    }
  }

  // Revoke sharers, return every owned block to the buddy, uncharge the
  // uid, unregister from the pid registry. p->as itself is not touched:
  // it is drained and dies wholesale on the next line.
  umem_destroy_process(p);
  as_free(p->as);
  printf("reaper: process %llu destroyed (memory revoked and freed)\n",
         p->pid);
  free(p);
}

static void reap_thread(struct thread *t) {
  asserts(t->status == THREAD_DEAD, "reaper: thread not dead");
  struct process *proc = t->proc;

  // Ring first: the worker may hold pointers into the shared block, so it
  // must be fully stopped before that block can go back to the buddy.
  if (t->ring != nullptr) {
    struct ring_shared *sh = ring_shared_of(t->ring);
    ring_destroy(t->ring);
    // The shared page is an ordinary ublock owned by the process; free it
    // now that nothing kernel-side references it. (If the whole process
    // is dying, doing it here vs. in umem_destroy_process is equivalent.)
    umem_free(proc, (uint64_t)sh, 0);
  }

  // Kernel threads own a task stack; user threads are stackless in the
  // kernel (their state is the TCB frame).
  if (proc->uid == 0 && t->stack_top != nullptr) {
    stacks_free_kernel(t->stack_top, STACK_TYPE_KERNEL_TASK);
  }
  free(t);

  if (proc != g_kernel_process &&
      atomic_fetch_sub(&proc->nthreads, 1) == 1) {
    process_destroy(proc);
  }
}

static void reaper_thread(void *arg) {
  (void)arg;
  while (true) {
    spinlock_lock(&g_lock);
    if (list_thread_ptr_len(g_dead) == 0) {
      g_reaper_sleeping = thread_current();
      spinlock_unlock(&g_lock);
      thread_block();
      continue;
    }
    struct thread *t;
    list_thread_ptr_pop_front(g_dead, &t);
    spinlock_unlock(&g_lock);
    reap_thread(t);
  }
}

void reaper_init(void) {
  spinlock_init(&g_lock);
  list_thread_ptr_new(&g_dead);
  g_reaper_sleeping = nullptr;
  kthread_spawn(reaper_thread, nullptr);
}
