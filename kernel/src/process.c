#include "process.h"

#include <stdatomic.h>

#include "channel.h"
#include "capability.h"
#include "cpu_state.h"
#include "debug.h"
#include "futex.h"
#include "iommu.h"
#include "irq_scheme.h"
#include "paging.h"
#include "scheduler.h"
#include "stdlib/stdio.h"
#include "stdlib/stdlib.h"
#include "syscall.h"
#include "thread.h"
#include "umem.h"
#include "uaccess.h"

#include <gdosabi/thread.h>

// Monotonic, never reused: 2^63 pids outlast the hardware, so a
// (pid, base) pair is forever unambiguous (no ABA on stale references).
static _Atomic uint64_t g_next_pid = 1;

static struct process *g_init;

static struct thread *process_spawn_thread_locked(struct process *p,
                                                  uint64_t entry,
                                                  uint64_t stack_pointer,
                                                  uint64_t arg,
                                                  uint64_t fs_base,
                                                  uint64_t gs_base,
                                                  ublock *completion_block,
                                                  uint64_t completion_event);

#define REAP_TCB_BATCH 256

void process_set_init(struct process *p) {
  asserts(g_init == nullptr, "process: init set twice");
  g_init = p;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

struct process *process_create(struct process *parent) {
  struct process *p = slab_process_zalloc(sizeof(*p));
  asserts(p != nullptr, "process: alloc failed");
  p->pid = atomic_fetch_add(&g_next_pid, 1);
  // Same identity layout as every AS (SASOS), but a private tree cloned
  // from the live kernel AS — which is boot-static now that nothing
  // punches kthread-stack guards into it. Isolation is which tree
  // carries PAGE_U leaves, enforced by the per-process CR3.
  p->as = as_clone(g_as_kernel);
  p->state = PROC_EMBRYO;
  p->parent = parent;
  vec_process_ptr_new(&p->children);
  vec_thread_ptr_new(&p->threads);
  umem_process_register(p); // enters the live-pid index
  if (parent != nullptr) {
    umem_lock();
    vec_process_ptr_push(parent->children, &p);
    umem_unlock();
  }
  return p;
}

struct thread *process_spawn_thread(struct process *p,
                                    const struct gdos_thread_start *start) {
  umem_lock();
  struct thread *t = process_spawn_thread_locked(
      p, start->entry, start->stack_pointer, start->argument, start->fs_base,
      start->gs_base, nullptr, 0);
  umem_unlock();
  return t;
}

static struct thread *process_spawn_thread_locked(struct process *p,
                                                  uint64_t entry,
                                                  uint64_t stack_pointer,
                                                  uint64_t arg,
                                                  uint64_t fs_base,
                                                  uint64_t gs_base,
                                                  ublock *completion_block,
                                                  uint64_t completion_event) {
  asserts(!process_is_dead(p), "process: spawning into the dead");
  // First spawn seals the embryo: parent authority (VM_MOVE in,
  // parent-set protections) drops to normal peer.
  p->state = PROC_LIVE;
  // Construct privately, install every process/ABI field, then enqueue as
  // the final publication. The old order queued first and let a fast exit on
  // another CPU race remove_thread_ref before proc_slot/vector insertion.
  struct thread *t = uthread_spawn(p, entry, stack_pointer, arg, fs_base,
                                   gs_base, completion_block, completion_event);
  t->proc_slot = vec_thread_ptr_len(p->threads);
  vec_thread_ptr_push(p->threads, &t);
  atomic_fetch_add(&p->nthreads, 1);
  scheduler_enqueue(t);
  return t;
}

// ---------------------------------------------------------------------------
// Death: one direct mark; descendants observe it through their ancestry
// ---------------------------------------------------------------------------

bool process_is_dead(const struct process *p) {
  for (; p != nullptr; p = p->parent) {
    if (atomic_load_explicit(&p->state, memory_order_acquire) == PROC_DEAD) {
      return true;
    }
  }
  return false;
}

static void remove_thread_ref(struct process *p, const struct thread *t) {
  uint32_t i = t->proc_slot;
  uint32_t n = vec_thread_ptr_len(p->threads);
  asserts(i < n, "process: bad thread slot");
  struct thread *at, *last;
  vec_thread_ptr_get(p->threads, i, &at);
  vec_thread_ptr_get(p->threads, n - 1, &last);
  asserts(at == t, "process: thread slot mismatch");
  vec_thread_ptr_swap_and_pop(p->threads, i);
  if (i != n - 1) {
    last->proc_slot = i;
  }
}

// Post KEV_CHILD_DEAD for `child` to its parent's tree channel, or leave
// the child's death_notified bit clear for replay (channel.c) when the
// channel exists and has room. Skipped entirely when the parent is
// itself dying — the grandparent hears about the parent instead.
static void notify_parent_locked(struct process *child) {
  struct process *parent = child->parent;
  if (parent == nullptr || process_is_dead(parent)) {
    child->death_notified = true; // nobody to tell, ever
    return;
  }
  channel_child_dead_notify(parent, child);
}

static void mark_dead_locked(struct process *p) {
  if (process_is_dead(p)) {
    return;
  }
  asserts(p != g_init, "init died");
  p->state = PROC_DEAD;
  umem_proc_unregister_locked(p);
}

// Reap materializes effective death one process at a time. Interior
// descendants do not notify their also-dead parents.
static void materialize_dead_locked(struct process *p) {
  if (p->state == PROC_DEAD) {
    return;
  }
  asserts(process_is_dead(p), "process: materializing the live");
  p->state = PROC_DEAD;
  umem_proc_unregister_locked(p);
}

void process_kill_subtree(struct process *p) {
  umem_lock();
  mark_dead_locked(p);
  notify_parent_locked(p);
  umem_unlock();
}

void process_thread_exited(struct thread *t) {
  struct process *p = t->proc;
  umem_lock();
  // This hook is reached only after scheduler_loop release-stored on_cpu=false.
  // Therefore an acquiring joiner may reclaim the departed thread's stack and
  // TLS immediately after observing this publication.
  if (!process_is_dead(p) && t->completion_block != nullptr) {
    channel_thread_complete_locked(p, t->completion_block,
                                   t->completion_event);
  }
  remove_thread_ref(p, t);
  arch_thread_destroy(t);
  slab_thread_free(t);
  uint64_t left = atomic_fetch_sub(&p->nthreads, 1) - 1;
  if (left == 0 && !process_is_dead(p)) {
    // Natural death: the last thread exited. Cascades exactly like a
    // kill — children never outlive their parent (daemonize via init).
    mark_dead_locked(p);
    notify_parent_locked(p);
  }
  umem_unlock();
}

// ---------------------------------------------------------------------------
// Reaping: one bounded step per call, deepest zombie first
// ---------------------------------------------------------------------------

static uint64_t reap_step_locked(struct process *target,
                                 struct umem_release *rel) {
  asserts(target->state == PROC_DEAD, "reap: target not dead");

  // Post-order cursor: nothing ever reparents, so the dead subtree is
  // closed, and descending first-child-first always terminates at a
  // childless zombie. Reaping it eventually unlinks it from its parent,
  // which is how the cursor advances.
  struct process *z = target;
  while (vec_process_ptr_len(z->children) > 0) {
    vec_process_ptr_get(z->children, 0, &z);
    asserts(process_is_dead(z), "reap: live process inside dead subtree");
  }
  materialize_dead_locked(z);

  if (cap_reap_one_locked(z)) {
    return REAP_MORE;
  }

  if (iommu_reap_one_locked(z)) {
    return REAP_MORE;
  }
  if (irq_reap_one_locked(z)) {
    return REAP_MORE;
  }

  if (umem_reap_one_block_locked(z, rel)) {
    return REAP_MORE; // caller runs umem_release_finish after unlocking
  }
  if (umem_reap_one_view_locked(z)) {
    return REAP_MORE;
  }
  // Free a fixed batch of blocked TCBs from the vector tail. A
  // RUNNABLE/RUNNING/DEAD tail remains scheduler-owned and is left
  // alone. A parked thread's futex node holds a thread pointer, so the
  // TCB may be freed only by a reap that wins the PARKED -> CLAIMED
  // claim and removes the node first; a CLAIMED or MOVING thread belongs
  // to someone else for the moment, and off-CPU-and-blocked alone is no
  // longer a licence to free (futex-design.md §5, §6).
  uint32_t ntcbs = 0;
  while (ntcbs < REAP_TCB_BATCH && vec_thread_ptr_len(z->threads) > 0) {
    uint32_t i = vec_thread_ptr_len(z->threads) - 1;
    struct thread *t;
    vec_thread_ptr_get(z->threads, i, &t);
    if (atomic_load_explicit(&t->on_cpu, memory_order_acquire) ||
        atomic_load_explicit(&t->status, memory_order_acquire) !=
            THREAD_BLOCKED) {
      break;
    }
    enum futex_state ws =
        atomic_load_explicit(&t->wake_state, memory_order_acquire);
    if (ws == FUTEX_PARKED) {
      if (!futex_try_claim(t)) {
        break; // a waker or expiry owns it; retry contract reports progress
      }
      futex_reap_claimed(t);
    } else if (ws != FUTEX_REAPABLE) {
      // CLAIMED/MOVING (or a not-yet-published park): in someone else's
      // hands right now — the existing SYSERR_AGAIN retry covers it.
      break;
    }
    remove_thread_ref(z, t);
    arch_thread_destroy(t);
    slab_thread_free(t);
    atomic_fetch_sub(&z->nthreads, 1);
    ntcbs++;
  }
  if (ntcbs != 0) {
    return REAP_MORE;
  }
  if (atomic_load(&z->nthreads) != 0) {
    // Killed threads that haven't been culled yet (running until their
    // next kernel entry, or queued until dispatch). Their TCBs and this
    // struct must outlive them.
    return SYSERR_AGAIN;
  }
  if (!z->as_freed) {
    // Three drains gate the AS free, all of the same shape. Pins: an
    // in-flight block release may still be flushing this AS outside
    // g_umem (umem_release_finish); each pin lasts one lock-free flush
    // round, and once z's lists are empty no new pin can target z->as.
    if (as_has_pins(z->as)) {
      return SYSERR_AGAIN;
    }
    // No CPU may still have the page tree loaded. Idle CPUs switch to
    // g_as_kernel and dispatch switches per-thread, so with the threads
    // culled this drains promptly (a CPU-bound hostile thread is forced
    // through its death checkpoint by the preemption timer within one
    // quantum, so the nthreads gate above clears in bounded time).
    for (size_t i = 0; i < g_cpu_state_table_len; i++) {
      if (g_cpu_state_table[i].current_as == z->as) {
        return SYSERR_AGAIN;
      }
    }
    as_free(z->as);
    z->as = nullptr;
    z->as_freed = true;
    return REAP_MORE;
  }

  // All resources gone: unlink and free the metadata in one shot.
  bool done = z == target;
  if (z->parent != nullptr) {
    for (uint32_t i = 0; i < vec_process_ptr_len(z->parent->children); i++) {
      struct process *c;
      vec_process_ptr_get(z->parent->children, i, &c);
      if (c == z) {
        vec_process_ptr_swap_and_pop(z->parent->children, i);
        break;
      }
    }
  }
  umem_reap_finish_locked(z);
  vec_process_ptr_delete(&z->children);
  vec_thread_ptr_delete(&z->threads);
  slab_process_free(z);
  return done ? REAP_DONE : REAP_MORE;
}

uint64_t process_reap_step(struct process *target) {
  struct umem_release rel = {0};
  umem_lock();
  uint64_t rc = reap_step_locked(target, &rel);
  umem_unlock();
  // The flush round + buddy return of a freed block run with no locks
  // held — the reap syscall's bounded step includes them, the rest of
  // the machine doesn't.
  umem_release_finish(&rel);
  return rc;
}

// ---------------------------------------------------------------------------
// Syscall backends
// ---------------------------------------------------------------------------

uint64_t proc_sys_create(struct thread *curr) {
  struct process *child = process_create(curr->proc);
  return child->pid;
}

static bool canonical48(uint64_t address) {
  uint64_t high = address >> 47;
  return high == 0 || high == 0x1FFFF;
}

static bool user_page_has(struct process *p, uint64_t address,
                          paging_flags_t required) {
  paging_flags_t flags = 0;
  bool present = false;
  as_getinfo(p->as, address, &flags, &present);
  return present && (flags & required) == required;
}

uint64_t proc_sys_thread_spawn(struct thread *curr, uint64_t pid,
                               uint64_t start_ptr, uint64_t start_size) {
  if (start_size != sizeof(struct gdos_thread_start)) {
    return SYSERR_INVAL;
  }
  struct gdos_thread_start start;
  // Pin the caller's list across validation+copy so a sibling cannot free or
  // guard the descriptor between user_range_ok and the load.
  umem_proc_lock(curr->proc);
  bool readable = user_range_ok(curr->proc, start_ptr, sizeof(start), false);
  if (readable) {
    start = *(const struct gdos_thread_start *)start_ptr;
  }
  umem_proc_unlock(curr->proc);
  if (!readable) {
    return SYSERR_FAULT;
  }
  if (start.version != GDOS_THREAD_START_VERSION ||
      start.size != sizeof(start) || !canonical48(start.entry) ||
      !canonical48(start.stack_pointer) ||
      !canonical48(start.fs_base) || !canonical48(start.gs_base) ||
      start.stack_pointer % 16 != 8) {
    return SYSERR_INVAL;
  }

  umem_lock();
  struct process *me = curr->proc;
  struct process *target = umem_proc_lookup_locked(pid);
  bool authority = target == me ||
                   (target != nullptr && target->parent == me &&
                    target->state == PROC_EMBRYO);
  if (!authority ||
      !user_page_has(target, start.entry, PAGE_U | PAGE_R | PAGE_X) ||
      start.stack_pointer == 0 ||
      !user_page_has(target, start.stack_pointer - 1,
                     PAGE_U | PAGE_R | PAGE_W)) {
    umem_unlock();
    return SYSERR_INVAL;
  }

  ublock *completion_block = nullptr;
  umem_proc_lock(target);
  bool start_ok = true;
  if (start.completion_event != 0) {
    // Join completion is process-private. Cross-process first-thread
    // lifetime is represented by the tree channel instead.
    completion_block =
        target == me
            ? umem_view_locked(target, start.completion_event, sizeof(uint32_t))
            : nullptr;
    start_ok = start.completion_event % alignof(uint32_t) == 0 &&
               completion_block != nullptr &&
               completion_block->owner == target &&
               completion_block->backing == UBLOCK_RAM &&
               completion_block->ring == nullptr &&
               vec_share_edge_len(completion_block->sharers) == 0 &&
               user_range_ok(target, start.completion_event, sizeof(uint32_t),
                             true) &&
               *(volatile _Atomic uint32_t *)start.completion_event ==
                   GDOS_THREAD_PENDING;
    if (start_ok) {
      atomic_fetch_add(&completion_block->thread_pins, 1);
    }
  }
  umem_proc_unlock(target);
  if (!start_ok) {
    umem_unlock();
    return SYSERR_INVAL;
  }

  struct thread *t = process_spawn_thread_locked(
      target, start.entry, start.stack_pointer, start.argument, start.fs_base,
      start.gs_base, completion_block, start.completion_event);
  uint64_t tid = t->tid;
  umem_unlock();
  return tid;
}

[[noreturn]] void proc_sys_process_exit(struct thread *curr, uint64_t status) {
  umem_lock();
  curr->proc->exit_status = status;
  mark_dead_locked(curr->proc);
  notify_parent_locked(curr->proc);
  umem_unlock();
  uthread_park_exit();
}

uint64_t proc_sys_kill(struct thread *curr, uint64_t pid) {
  struct process *me = curr->proc;
  umem_lock();
  struct process *victim = umem_proc_lookup_locked(pid);
  if (victim == nullptr || victim == me) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  // Strict descendant: authority follows creation edges only.
  struct process *a = victim->parent;
  while (a != nullptr && a != me) {
    a = a->parent;
  }
  if (a != me) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  mark_dead_locked(victim);
  notify_parent_locked(victim);
  umem_unlock();
  return 0;
}

// Find `pid` among `me`'s children (zombies included — they left the
// live-pid index at death but stay tree-linked until reaped).
static struct process *find_child_locked(struct process *me, uint64_t pid) {
  for (uint32_t i = 0; i < vec_process_ptr_len(me->children); i++) {
    struct process *c;
    vec_process_ptr_get(me->children, i, &c);
    if (c->pid == pid) {
      return c;
    }
  }
  return nullptr;
}

uint64_t proc_sys_reap(struct thread *curr, uint64_t pid) {
  struct umem_release rel = {0};
  umem_lock();
  struct process *child = find_child_locked(curr->proc, pid);
  if (child == nullptr || child->state != PROC_DEAD) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  uint64_t rc = reap_step_locked(child, &rel);
  umem_unlock();
  umem_release_finish(&rel);
  return rc;
}

uint64_t proc_sys_vm_move(struct thread *curr, uint64_t base, uint64_t pid) {
  struct process *me = curr->proc;
  umem_lock();

  // Down: construction — into your own embryo.
  struct process *target = umem_proc_lookup_locked(pid);
  if (target != nullptr && target->parent == me &&
      target->state == PROC_EMBRYO) {
    umem_proc_lock(me);
    ublock *b = umem_owned_locked(me, base);
    umem_proc_unlock(me); // b stays pinned by g_umem
    if (b == nullptr || b->ring != nullptr) {
      umem_unlock();
      return SYSERR_INVAL;
    }
    uint64_t rc = umem_move_locked(b, me, target, true);
    umem_unlock();
    return rc;
  }

  // Up: reap-time claim — out of your own zombie child (post-mortem
  // inspection and exec-image recycling are libraries built on this).
  struct process *child = find_child_locked(me, pid);
  if (child != nullptr && child->state == PROC_DEAD) {
    umem_proc_lock(child);
    ublock *b = umem_owned_locked(child, base);
    umem_proc_unlock(child);
    if (b == nullptr || b->ring != nullptr) {
      umem_unlock();
      return SYSERR_INVAL;
    }
    uint64_t rc = umem_move_locked(b, child, me, !child->as_freed);
    umem_unlock();
    return rc;
  }

  umem_unlock();
  return SYSERR_INVAL;
}

uint64_t proc_sys_vm_protect_for(struct thread *curr, uint64_t base,
                                 uint64_t len, uint64_t prot, uint64_t pid) {
  // g_umem pins the looked-up embryo across the protect (which itself
  // only takes the target's list lock).
  umem_lock();
  struct process *target = umem_proc_lookup_locked(pid);
  if (target == nullptr || target->parent != curr->proc ||
      target->state != PROC_EMBRYO) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  int rc = umem_protect(target, base, len, (paging_flags_t)prot);
  umem_unlock();
  return rc == 0 ? 0 : SYSERR_PERM;
}
