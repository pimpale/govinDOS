#include "syscall.h"

#include <stdint.h>

#include "cpu_state.h"
#include "debug.h"
#include "dummydev.h"
#include "ring.h"
#include "thread.h"
#include "trap_frame.h"
#include "uaccess.h"

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

static uint64_t sys_vm_map(struct thread *curr, uint64_t len, uint64_t prot) {
  if (len == 0 || len > (64ull << 20) || (prot & ~7ull) != 0 ||
      !(prot & VM_PROT_READ)) {
    return SYSERR_INVAL;
  }
  paging_flags_t flags = PAGE_R;
  if (prot & VM_PROT_WRITE) {
    flags |= PAGE_W;
  }
  if (prot & VM_PROT_EXEC) {
    flags |= PAGE_X;
  }
  void *base = umem_alloc(curr->proc, len, flags);
  if (base == nullptr) {
    return SYSERR_NOMEM;
  }
  return (uint64_t)base;
}

static uint64_t sys_vm_unmap(struct thread *curr, uint64_t base,
                             uint64_t len) {
  if (umem_free(curr->proc, base, len) != 0) {
    return SYSERR_PERM;
  }
  return 0;
}

void syscall_entry(struct trap_frame *tf) {
  struct thread *curr = thread_current();
  asserts(curr != nullptr, "syscall: no current thread");

  // Kernel threads have in-kernel APIs for all of this; the parking ops
  // in particular assume a user thread whose state fits in a trap frame.
  bool from_user = (tf->cs & 3) == 3;

  uint64_t nr = tf->rax;
  uint64_t a0 = tf->rcx, a1 = tf->rdx, a2 = tf->r8, a3 = tf->r9;
  (void)a2;
  (void)a3;

  switch (nr) {
  case SYS_DEBUG_WRITE:
    tf->rax = sys_debug_write(curr, a0, a1);
    return;

  case SYS_GETUID:
    tf->rax = curr->proc->uid;
    return;

  case SYS_GETPID:
    tf->rax = curr->proc->pid;
    return;

  case SYS_VM_MAP:
    tf->rax = sys_vm_map(curr, a0, a1);
    return;

  case SYS_VM_UNMAP:
    tf->rax = sys_vm_unmap(curr, a0, a1);
    return;

  case SYS_EXIT:
    if (!from_user) {
      tf->rax = SYSERR_PERM;
      return;
    }
    uthread_park_exit();

  case SYS_YIELD:
    if (!from_user) {
      tf->rax = SYSERR_PERM;
      return;
    }
    // The saved frame is what we resume from — bake the return value in
    // before capturing it.
    tf->rax = 0;
    arch_uthread_save_frame(curr, tf);
    uthread_park_yield();

  case SYS_DUMMY_READ:
    if (!from_user) {
      tf->rax = SYSERR_PERM;
      return;
    }
    // Result is delivered into the saved frame's rax by the device
    // worker (thread_deliver_wait_result) before it unblocks us.
    arch_uthread_save_frame(curr, tf);
    dummydev_read_user(curr);

  case SYS_RING_CREATE:
    tf->rax = ring_create(curr);
    return;

  case SYS_RING_ENTER:
    tf->rax = ring_enter(curr);
    return;

  case SYS_RING_WAIT:
    if (!from_user) {
      tf->rax = SYSERR_PERM;
      return;
    }
    // May park (frame must be saved first, with the return value baked
    // in) or return immediately through the live frame.
    tf->rax = 0;
    arch_uthread_save_frame(curr, tf);
    tf->rax = ring_wait_user(curr, (uint32_t)a0);
    return;

  default:
    tf->rax = SYSERR_NOSYS;
    return;
  }
}
