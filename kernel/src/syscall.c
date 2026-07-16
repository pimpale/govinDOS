#include "syscall.h"

#include <stdint.h>

#include "channel.h"
#include "capability.h"
#include "cpu_state.h"
#include "debug.h"
#include "irq.h"
#include "process.h"
#include "thread.h"
#include "trap_frame.h"
#include "uaccess.h"
#include "umem.h"

// serial-backed character sink used by printf (stdio.c)
extern void putchar_(char c);

static uint64_t sys_debug_write(struct thread *curr, uint64_t uptr,
                                uint64_t len) {
  if (len > 4096) {
    return SYSERR_INVAL;
  }
  if (!user_range_ok(curr->proc, uptr, len, false)) {
    return SYSERR_FAULT;
  }
  const char *s = (const char *)uptr;
  for (uint64_t i = 0; i < len; i++) {
    putchar_(s[i]);
  }
  return len;
}

// Translate user VM_PROT_* bits to paging flags. Callers validate the
// bits first; prot 0 (vm_protect's guard view) maps to empty flags.
static paging_flags_t vm_prot_to_flags(uint64_t prot) {
  paging_flags_t flags = 0;
  if (prot & VM_PROT_READ) {
    flags |= PAGE_R;
  }
  if (prot & VM_PROT_WRITE) {
    flags |= PAGE_W;
  }
  if (prot & VM_PROT_EXEC) {
    flags |= PAGE_X;
  }
  return flags;
}

// prot must be within the known bits and, unless allow_none, include R.
static bool vm_prot_ok(uint64_t prot, bool allow_none) {
  if ((prot & ~7ull) != 0) {
    return false;
  }
  if (prot == 0) {
    return allow_none;
  }
  return (prot & VM_PROT_READ) != 0;
}

static uint64_t sys_vm_alloc(struct thread *curr, uint64_t len,
                             uint64_t prot) {
  if (len == 0 || len > (64ull << 20) || !vm_prot_ok(prot, false)) {
    return SYSERR_INVAL;
  }
  void *base = umem_alloc(curr->proc, len, vm_prot_to_flags(prot));
  if (base == nullptr) {
    return SYSERR_NOMEM;
  }
  return (uint64_t)base;
}

static uint64_t sys_vm_free(struct thread *curr, uint64_t base) {
  int rc = umem_free(curr->proc, base);
  if (rc == (int)SYSERR_EXIST)
    return SYSERR_EXIST;
  if (rc != 0) {
    return SYSERR_PERM;
  }
  return 0;
}

static uint64_t sys_vm_size(struct thread *curr, uint64_t base) {
  uint64_t bytes = umem_size(curr->proc, base);
  return bytes == 0 ? SYSERR_PERM : bytes;
}

static uint64_t sys_vm_protect(struct thread *curr, uint64_t base,
                               uint64_t len, uint64_t prot, uint64_t pid) {
  if (!vm_prot_ok(prot, true)) {
    return SYSERR_INVAL;
  }
  if (pid != 0 && pid != curr->proc->pid) {
    // Parent applying view flags (W^X on a written image) to its own
    // embryo — the one window where another process's views are yours
    // to set.
    return proc_sys_vm_protect_for(curr, base, len, vm_prot_to_flags(prot),
                                   pid);
  }
  if (umem_protect(curr->proc, base, len, vm_prot_to_flags(prot)) != 0) {
    return SYSERR_PERM;
  }
  return 0;
}

static uint64_t sys_vm_share(struct thread *curr, uint64_t base,
                             uint64_t target, uint64_t prot) {
  // Signed target: a user pid maps the block into that process on the
  // spot; a negative id turns the block into a kernel channel of that
  // scheme (prot is meaningless there — the layout is kernel ABI).
  if ((int64_t)target < 0) {
    return channel_scheme_create(curr->proc, base, (int64_t)target);
  }
  if (!vm_prot_ok(prot, false)) {
    return SYSERR_INVAL;
  }
  return umem_share(curr->proc, base, target, vm_prot_to_flags(prot));
}

void syscall_entry_from_user(struct trap_frame *tf) {
  irq_enter();
  syscall_entry(tf);
  irq_exit();
}

void syscall_entry(struct trap_frame *tf) {
  struct thread *curr = thread_current();
  asserts(curr != nullptr, "syscall: no current thread");

  // Death checkpoint: direct or inherited death takes effect at the next
  // kernel entry. channel_block_wait repeats it before installing a slot.
  if (process_is_dead(curr->proc)) {
    uthread_park_exit();
  }

  uint64_t nr = tf->rax;
  uint64_t a0 = tf->rcx, a1 = tf->rdx, a2 = tf->r8, a3 = tf->r9;

  switch (nr) {
  case SYS_DEBUG_WRITE:
    tf->rax = sys_debug_write(curr, a0, a1);
    return;

  case SYS_GETPID:
    tf->rax = curr->proc->pid;
    return;

  case SYS_VM_ALLOC:
    tf->rax = sys_vm_alloc(curr, a0, a1);
    return;

  case SYS_VM_FREE:
    tf->rax = sys_vm_free(curr, a0);
    return;

  case SYS_VM_PROTECT:
    tf->rax = sys_vm_protect(curr, a0, a1, a2, a3);
    return;

  case SYS_VM_SHARE:
    tf->rax = sys_vm_share(curr, a0, a1, a2);
    return;

  case SYS_VM_UNSHARE:
    tf->rax = umem_unshare(curr->proc, a0) == 0 ? 0 : SYSERR_PERM;
    return;

  case SYS_VM_MOVE:
    tf->rax = proc_sys_vm_move(curr, a0, a1);
    return;

  case SYS_VM_MAP_DEVICE:
    tf->rax = cap_map_device(curr->proc, a0, a1, (uint32_t)a2);
    return;

  case SYS_VM_SIZE:
    tf->rax = sys_vm_size(curr, a0);
    return;

  case SYS_BLOCK_DOORBELL:
    tf->rax = channel_block_doorbell(curr, a0);
    return;

  case SYS_BLOCK_WAIT:
    // May park (frame saved first, success value 0 baked in — a waker
    // that isn't plain success overwrites the saved rax) or return
    // immediately through the live frame.
    tf->rax = 0;
    arch_uthread_save_frame(curr, tf);
    tf->rax = channel_block_wait(curr, a0, a1);
    return;

  case SYS_PROC_CREATE:
    tf->rax = proc_sys_create(curr);
    return;

  case SYS_THREAD_SPAWN:
    tf->rax = proc_sys_thread_spawn(curr, a0, a1, a2, a3);
    return;

  case SYS_PROC_KILL:
    tf->rax = proc_sys_kill(curr, a0);
    return;

  case SYS_PROC_REAP:
    tf->rax = proc_sys_reap(curr, a0);
    return;

  case SYS_EXIT:
    uthread_park_exit();

  case SYS_YIELD:
    // The saved frame is what we resume from — bake the return value in
    // before capturing it.
    tf->rax = 0;
    arch_uthread_save_frame(curr, tf);
    uthread_park_yield();

  default:
    tf->rax = SYSERR_NOSYS;
    return;
  }
}
