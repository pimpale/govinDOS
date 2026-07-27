#ifndef scheduler_h_INCLUDED
#define scheduler_h_INCLUDED

#include <stdatomic.h>

#include "spinlock.h"
#include "thread.h"
#include "timer_queue.h"

#define LIST_DTYPE thread_ptr
#include <list/list.h>
#undef LIST_DTYPE

// IDT vector for the cross-CPU reschedule IPI. Value is arbitrary as long
// as it doesn't collide with any other installed vector (currently
// VECTOR_TLB_SHOOTDOWN=0xFD and VECTOR_TIMER=0xFB are the others). The
// handler is a no-op + EOI; its only job is to wake a HLT'd CPU so its
// scheduler loop iterates and picks up newly-enqueued work.
#define VECTOR_RESCHED 0xFC

// IDT vector for the per-CPU LAPIC one-shot shared by scheduling quanta and
// parked threads' futex deadlines. The earliest absolute deadline wins.
// Kernel code runs IRQs-off, so delivery normally lands in ring 3 or the
// scheduler idle path.
#define VECTOR_TIMER 0xFB

// Length of one dispatch quantum. A thread that neither parks nor blocks
// for this long is preempted as if it had called SYS_YIELD.
#define SCHED_QUANTUM_US 10000

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
  // Currently running thread on this CPU. nullptr means the per-CPU
  // scheduler loop is running (between threads). Set by the scheduler
  // before switching into a thread; cleared after the thread switches
  // back out.
  struct thread *current_thread;
  list_thread_ptr *queue;
  struct spinlock lock;
  uint64_t sched_rsp;
  _Atomic bool idle;

  // One local-APIC timer is multiplexed between this CPU's scheduling
  // quantum and the earliest parked-thread deadline (timer.c). The tree
  // holds parked threads and nothing else; arming is always local, and
  // the lock nests inside futex buckets (bucket -> timer everywhere).
  // timer_lock is never held while scheduler.lock is held.
  struct spinlock timer_lock;
  llrb_tdeadline *deadlines;
  uint64_t quantum_deadline_ns; // zero outside a dispatched quantum
};

// One-time global init. Allocates each CPU's queue and initializes its
// lock. Must run on the BSP after cpu_state_table_init.
void scheduler_init(void);

// Pop the head of THIS CPU's runqueue, or nullptr if empty.
// Caller must hold scheduler->lock and have IRQs disabled.
struct thread *scheduler_pop_local_locked(struct scheduler *scheduler);

// Push `t` at the tail of THIS CPU's runqueue.
// Caller must hold scheduler->lock and have IRQs disabled.
void scheduler_push_local_locked(struct scheduler *scheduler,
                                 struct thread *t);

// Cross-CPU enqueue. Selects a target CPU via internal placement policy
// (round-robin today) and pushes `t` onto that CPU's queue. Acquires the
// target's lock briefly; safe to call from any CPU.
void scheduler_enqueue(struct thread *t);

// Per-CPU entry point into the scheduler.
// Will repeatedly schedule threads, and sleep if no thread is available to run.
[[noreturn]] void scheduler_loop(struct scheduler *scheduler);

#endif // scheduler_h_INCLUDED
