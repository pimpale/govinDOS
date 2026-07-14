# Userspace Implementation of C Standard Library

Not all functions are included!

Uses the public `<gdos/sys.h>` syscall wrappers for allocation, exit, and
debug-console output. It does not require `gdoslib` at link time.

The allocator is deliberately stateless: each allocation is backed directly by
`sys_vm_alloc`, `realloc` obtains its usable capacity from `sys_vm_size`, and
`free` releases the corresponding VM block. Locking is unnecessary because
there is no shared allocator state.
