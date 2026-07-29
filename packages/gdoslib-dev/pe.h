#ifndef pe_h_INCLUDED
#define pe_h_INCLUDED

// Userland PE32+ loader: kernel/src/pe.c's logic as a library, run over
// syscalls (ipc-process-design.md §5). The kernel's own loader remains
// boot-only (it loads exactly init); every other process is built by its
// parent through this path:
//
//   VM_ALLOC a block, write headers + sections, relocate against the
//   block's address (SASOS: the child will see the same addresses),
//   PROC_CREATE a zero-thread child, VM_MOVE it the image and a stack, set
//   per-section W^X on the child's views, THREAD_SPAWN.

#include <stdbool.h>
#include <stdint.h>

// Build a child process from the PE image at image[0..len) and start its
// first thread with `arg`. stack_len is the stack block size in bytes
// (power-of-two pages). Returns the child pid, or 0 on failure (bad
// image / out of memory), with the reason printed.
uint64_t pe_spawn(const uint8_t *image, uint64_t len, uint64_t arg,
                  uint64_t stack_len);

struct pe_resource {
  uint64_t base;
  uint64_t prot;
};

// Allocate and initialize the current process's one-module PE TLS runtime
// (minimal TEB prefix, TLS vector slot zero, and a copy of the image's static
// TLS template). `image_base` names an already relocated in-memory PE image.
// Returns the GSBASE to place in gdos_thread_start, or zero on failure.
uint64_t pe_tls_create(uint64_t image_base);

// As pe_spawn, but establishes read/write views of parent-owned blocks in the
// child before its first thread runs.
uint64_t pe_spawn_resources(const uint8_t *image, uint64_t len, uint64_t arg,
                            uint64_t stack_len,
                            const struct pe_resource *resources,
                            uint32_t nresources);

// Explicitly dismantle and destroy a dead descendant subtree using the
// enumeration ABI. Returns true only after every process body is gone.
bool pe_destroy(uint64_t pid);

#endif // pe_h_INCLUDED
