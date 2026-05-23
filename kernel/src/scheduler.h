#ifndef scheduler_h_INCLUDED
#define scheduler_h_INCLUDED

#include <stdatomic.h>

#include "spinlock.h"
#include "thread.h"

#define LIST_DTYPE thread_ptr
#include <list/list.h>
#undef LIST_DTYPE

struct cpu_state;

// IDT vector for the cross-CPU reschedule IPI. Value is arbitrary as long
// as it doesn't collide with any other installed vector (currently only
// VECTOR_TLB_SHOOTDOWN=0xFD and the syscall 0x80 are claimed). The
// handler is a no-op + EOI; its only job is to wake a HLT'd CPU so its
// scheduler loop iterates and picks up newly-enqueued work.
#define VECTOR_RESCHED 0xFC

// Per-CPU scheduler state. Embedded by value in struct cpu_state, so
// scheduler.h is included by cpu_state.h.
//   queue, lock:  this CPU's runqueue and the lock guarding it.
//   sched_rsp:    saved kernel SP of the per-CPU scheduler loop running
//                 on the bootstrap stack; threads switch back to it via
//                 switch_context on yield/block/exit.
//   idle:         true while this CPU is parked at sti;hlt waiting for
//                 work. Set/read under `lock` so a producer that pushes
//                 onto an empty queue can decide whether to send an IPI.
//                 Cleared by the consumer after waking (lock not held).
struct scheduler {
  list_thread_ptr *queue;
  struct spinlock lock;
  uint64_t sched_rsp;
  _Atomic bool idle;
};

// One-time global init. Allocates each CPU's queue and initializes its
// lock. Must run on the BSP after cpu_state_table_init.
void scheduler_init(void);

// Pop the head of THIS CPU's runqueue, or nullptr if empty.
// Caller must hold cs->scheduler.lock and have IRQs disabled.
struct thread *scheduler_pop_local_locked(struct cpu_state *cs);

// Push `t` at the tail of THIS CPU's runqueue.
// Caller must hold cs->scheduler.lock and have IRQs disabled.
void scheduler_push_local_locked(struct cpu_state *cs, struct thread *t);

// Cross-CPU enqueue. Selects a target CPU via internal placement policy
// (round-robin today) and pushes `t` onto that CPU's queue. Acquires the
// target's lock briefly; safe to call from any CPU.
void scheduler_enqueue(struct thread *t);

#endif // scheduler_h_INCLUDED
