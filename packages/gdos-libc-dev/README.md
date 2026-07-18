# Userspace Implementation of C Standard Library

This is a deliberately growing freestanding libc, not yet a complete ISO C or
POSIX implementation.

Uses the public `<gdos/sys.h>` syscall wrappers for allocation, exit, and
debug-console output. It does not require `gdoslib` at link time.

The allocator is deliberately stateless: each allocation is backed directly by
`sys_vm_alloc`, `realloc` obtains its usable capacity from `sys_vm_size`, and
`free` drives resumable `SYS_VM_FREE` calls through `SYSERR_AGAIN` until the
kernel has notified every waiter and reclaimed the block. Locking is
unnecessary because there is no shared allocator state.

The basic pthread surface includes thread creation/join/detach, independent PE
TLS, attributes, mutexes, condition variables, once, rwlocks, spinlocks,
barriers, cleanup handlers, and thread-specific data destructors. Private
synchronization waits use the kernel's multi-waiter private-block FIFO. Wakeups
are block-scoped, so libc treats them as spurious and rechecks the atomic
predicate. Timed waits, process-shared synchronization, and cancellation are
currently unsupported; those operations return `ENOTSUP` where applicable.

The basic libc surface also includes ASCII ctype, errno, common string and
case-insensitive string operations, integer conversion, sorting/searching,
random numbers, console `write`/`puts`/`perror`, process/thread ids, and yield.
There is not yet a filesystem-backed `FILE` implementation, locale, signals,
or wall-clock service.
