#ifndef upe_h_INCLUDED
#define upe_h_INCLUDED

// Userland PE32+ loader: kernel/src/pe.c's logic as a library, run over
// syscalls (ipc-process-design.md §5). The kernel's own loader remains
// boot-only (it loads exactly init); every other process is built by its
// parent through this path:
//
//   VM_MAP a block, write headers + sections, relocate against the
//   block's address (SASOS: the child will see the same addresses),
//   PROC_CREATE an embryo, VM_MOVE it the image and a stack, set
//   per-section W^X on the embryo's views, THREAD_SPAWN.

#include <stdint.h>

// Build a child process from the PE image at image[0..len) and start its
// first thread with `arg`. stack_len is the stack block size in bytes
// (power-of-two pages). Returns the child pid, or 0 on failure (bad
// image / out of memory), with the reason printed.
uint64_t upe_spawn(const uint8_t *image, uint64_t len, uint64_t arg,
                   uint64_t stack_len);

#endif // upe_h_INCLUDED
