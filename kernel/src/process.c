#include "process.h"

#include <stdatomic.h>

#include "channel.h"
#include "cpu_state.h"
#include "debug.h"
#include "futex.h"
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
  p->parent = parent;
  asserts(llrb_pid_process_new(&p->children),
          "process: children tree alloc failed");
  asserts(llrb_tid_thread_new(&p->threads),
          "process: thread tree alloc failed");
  umem_process_register(p); // enters the live-pid index
  if (parent != nullptr) {
    umem_lock();
    asserts(llrb_pid_process_insert(parent->children, &p->pid, &p),
            "process: child insertion failed");
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
  // Construct privately, install every process/ABI field, then enqueue as
  // the final publication. The old order queued first and let a fast exit on
  // another CPU race remove_thread_ref before thread-index insertion.
  struct thread *t = uthread_spawn(p, entry, stack_pointer, arg, fs_base,
                                   gs_base, completion_block, completion_event);
  asserts(llrb_tid_thread_insert(p->threads, &t->tid, &t),
          "process: thread insertion failed");
  atomic_fetch_add(&p->nthreads, 1);
  scheduler_enqueue(t);
  return t;
}

// ---------------------------------------------------------------------------
// Death: one direct mark; descendants observe it through their ancestry
// ---------------------------------------------------------------------------

bool process_is_dead(const struct process *p) {
  for (; p != nullptr; p = p->parent) {
    if (atomic_load_explicit(&p->dead, memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

static void remove_thread_ref(struct process *p, const struct thread *t) {
  struct thread *removed;
  asserts(llrb_tid_thread_remove(p->threads, &t->tid, &removed) &&
              removed == t,
          "process: thread index mismatch");
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
  p->dead = true;
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
  if (t->completion_block != nullptr) {
    channel_thread_complete_locked(p, t->completion_block,
                                   t->completion_event, !process_is_dead(p));
    t->completion_block = nullptr;
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

// Reaper authority is a strict descendant path whose every intervening
// process is effectively dead. A live child is therefore a hard boundary:
// its parent cannot skip through it to manage a dead grandchild.
bool process_reaper_authorized_locked(const struct process *caller,
                                      const struct process *target) {
  if (caller == target || !process_is_dead(target))
    return false;
  for (const struct process *at = target; at != nullptr && at != caller;
       at = at->parent) {
    if (!process_is_dead(at))
      return false;
    if (at->parent == caller)
      return true;
  }
  return false;
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
  bool authority =
      target == me || (target != nullptr && target->parent == me);
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
    // Join completion is process-private. A parent tracks child lifetime
    // through the tree channel instead.
    completion_block =
        target == me
            ? umem_view_locked(target, start.completion_event, sizeof(uint32_t))
            : nullptr;
    start_ok = start.completion_event % alignof(uint32_t) == 0 &&
               completion_block != nullptr &&
               completion_block->owner == target &&
               completion_block->backing == UBLOCK_RAM &&
               completion_block->ring == nullptr &&
               llrb_pid_edge_len(completion_block->sharers) == 0 &&
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

static struct process *resource_subject_locked(struct process *caller,
                                               uint64_t pid) {
  if (pid == 0 || pid == caller->pid)
    return caller;
  struct process *target = umem_proc_lookup_any_locked(pid);
  return target != nullptr &&
                 process_reaper_authorized_locked(caller, target)
             ? target
             : nullptr;
}

uint64_t proc_sys_threads(struct thread *curr, uint64_t pid, uint64_t buf,
                          uint64_t cap, uint64_t after) {
  if (cap == 0 || cap > VM_ENUM_BATCH)
    return SYSERR_INVAL;
  uint64_t values[VM_ENUM_BATCH];
  uint64_t count = 0;
  umem_lock();
  struct process *target = resource_subject_locked(curr->proc, pid);
  if (target == nullptr) {
    umem_unlock();
    return SYSERR_PERM;
  }
  llrb_tid_thread_iter iter;
  llrb_tid_thread_iter_lower_bound(target->threads, &after, &iter);
  uint64_t key;
  while (count < cap && llrb_tid_thread_iter_next(&iter, &key, nullptr))
    if (key > after)
      values[count++] = key;
  uint64_t rc =
      umem_enum_copyout_locked(curr->proc, buf, cap, values, count);
  umem_unlock();
  return rc;
}

uint64_t proc_sys_children(struct thread *curr, uint64_t pid, uint64_t buf,
                           uint64_t cap, uint64_t after) {
  if (cap == 0 || cap > VM_ENUM_BATCH)
    return SYSERR_INVAL;
  uint64_t values[VM_ENUM_BATCH];
  uint64_t count = 0;
  umem_lock();
  struct process *target = resource_subject_locked(curr->proc, pid);
  if (target == nullptr) {
    umem_unlock();
    return SYSERR_PERM;
  }
  llrb_pid_process_iter iter;
  llrb_pid_process_iter_lower_bound(target->children, &after, &iter);
  uint64_t key;
  while (count < cap &&
         llrb_pid_process_iter_next(&iter, &key, nullptr))
    if (key > after)
      values[count++] = key;
  uint64_t rc =
      umem_enum_copyout_locked(curr->proc, buf, cap, values, count);
  umem_unlock();
  return rc;
}

uint64_t proc_sys_thread_destroy(struct thread *curr, uint64_t pid,
                                 uint64_t tid) {
  umem_lock();
  struct process *target = resource_subject_locked(curr->proc, pid);
  if (target == nullptr ||
      (target != curr->proc && !process_is_dead(target))) {
    umem_unlock();
    return SYSERR_PERM;
  }
  struct thread *victim;
  if (!llrb_tid_thread_get(target->threads, &tid, &victim)) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  if (atomic_load_explicit(&victim->on_cpu, memory_order_acquire) ||
      atomic_load_explicit(&victim->status, memory_order_acquire) !=
          THREAD_BLOCKED) {
    umem_unlock();
    return SYSERR_AGAIN;
  }
  enum futex_state ws =
      atomic_load_explicit(&victim->wake_state, memory_order_acquire);
  if (ws == FUTEX_PARKED) {
    if (!futex_try_claim(victim)) {
      umem_unlock();
      return SYSERR_AGAIN;
    }
    futex_reap_claimed(victim);
  } else if (ws != FUTEX_REAPABLE) {
    umem_unlock();
    return SYSERR_AGAIN;
  }
  if (victim->completion_block != nullptr) {
    channel_thread_complete_locked(target, victim->completion_block,
                                   victim->completion_event, false);
    victim->completion_block = nullptr;
  }
  remove_thread_ref(target, victim);
  arch_thread_destroy(victim);
  slab_thread_free(victim);
  atomic_fetch_sub(&target->nthreads, 1);
  umem_unlock();
  return 0;
}

uint64_t proc_sys_destroy(struct thread *curr, uint64_t pid) {
  umem_lock();
  struct process *target = umem_proc_lookup_any_locked(pid);
  if (target == nullptr ||
      !process_reaper_authorized_locked(curr->proc, target)) {
    umem_unlock();
    return SYSERR_PERM;
  }
  umem_proc_lock(target);
  bool has_memory = llrb_base_block_len(target->blocks) != 0 ||
                    llrb_base_edge_len(target->views) != 0;
  umem_proc_unlock(target);
  if (has_memory || llrb_pid_process_len(target->children) != 0 ||
      llrb_tid_thread_len(target->threads) != 0) {
    umem_unlock();
    return SYSERR_EXIST;
  }
  if (target->as != nullptr) {
    if (as_has_pins(target->as)) {
      umem_unlock();
      return SYSERR_AGAIN;
    }
    for (size_t i = 0; i < g_cpu_state_table_len; i++) {
      if (g_cpu_state_table[i].current_as == target->as) {
        umem_unlock();
        return SYSERR_AGAIN;
      }
    }
    as_free(target->as);
    target->as = nullptr;
    umem_unlock();
    return PROC_DESTROY_MORE;
  }
  struct process *parent = target->parent;
  struct process *removed;
  asserts(llrb_pid_process_remove(parent->children, &target->pid, &removed) &&
              removed == target,
          "process: child missing during destroy");
  umem_proc_unregister_locked(target);
  umem_process_finish_locked(target);
  llrb_pid_process_delete(&target->children);
  llrb_tid_thread_delete(&target->threads);
  slab_process_free(target);
  umem_unlock();
  return PROC_DESTROY_DONE;
}

uint64_t proc_sys_vm_move(struct thread *curr, uint64_t base, uint64_t pid) {
  struct process *me = curr->proc;
  umem_lock();

  // A direct parent retains configuration authority for the child's
  // lifetime. Page-table changes are safe while either endpoint runs:
  // umem_move_locked performs the mapping transition and shootdown under
  // g_umem.
  struct process *target = umem_proc_lookup_locked(pid);
  if (target != nullptr && target->parent == me) {
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

  umem_unlock();
  return SYSERR_INVAL;
}

uint64_t proc_sys_vm_protect_for(struct thread *curr, uint64_t base,
                                 uint64_t len, uint64_t prot, uint64_t pid) {
  // g_umem pins the live direct child across the protect (which itself
  // takes the target's list lock and performs the required shootdown).
  umem_lock();
  struct process *target = umem_proc_lookup_locked(pid);
  if (target == nullptr || target->parent != curr->proc) {
    umem_unlock();
    return SYSERR_INVAL;
  }
  int rc = umem_protect(target, base, len, (paging_flags_t)prot);
  umem_unlock();
  return rc == 0 ? 0 : SYSERR_PERM;
}
