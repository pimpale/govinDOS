#ifndef reaper_h_INCLUDED
#define reaper_h_INCLUDED

// The reaper kthread: owns all post-mortem teardown.
//
//   dead thread  -> ring teardown (stop worker, free ring struct + shared
//                   block), kernel stack (guard restore + free), TCB.
//   last thread of a user process -> process teardown: wait for the
//                   process AS to drain off every CPU (the scheduler
//                   switches to g_as_kernel when idling and to the next
//                   thread's AS when dispatching, so this terminates),
//                   then revoke/free all its user memory, as_free the
//                   page tree, and free the process struct.
//
// Teardown runs on a kthread rather than in the scheduler loop because it
// blocks (drain waits, worker-stop waits) and takes locks the IRQs-off
// scheduler path must not hold.

struct thread;
struct process;

// One-time init: allocates the dead list and spawns the reaper kthread.
// Must run after threading_init and before the first thread can die.
void reaper_init(void);

// Hand a dead thread to the reaper. Called by the scheduler loop after
// the thread's context save is complete (on_cpu already false). IRQs-off
// safe; takes only the reaper's own lock.
void reaper_submit(struct thread *t);

// Synchronous process teardown for a user process that has no threads
// (and never had any scheduled) — used by boot-time self-tests. Regular
// process death goes through reaper_submit via the last thread's exit.
void process_destroy(struct process *p);

#endif // reaper_h_INCLUDED
