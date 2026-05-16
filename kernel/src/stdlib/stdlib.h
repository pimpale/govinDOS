#ifndef stdlib_h_INCLUDED
#define stdlib_h_INCLUDED

#include <stddef.h>
#include <stdint.h>

struct efi_memory_descriptor;

// Build a buddy allocator covering all EFI_CONVENTIONAL_MEMORY pages in the
// given EFI memory map and install it as the global heap. Must be called once
// during kernel init before any allocation.
void kstdlib_init(uint64_t n_mmap, const struct efi_memory_descriptor *mmap);

[[nodiscard]] void *malloc(size_t size);
[[nodiscard]] void *calloc(size_t nmemb, size_t size);
[[nodiscard]] void *realloc(void *ptr, size_t size);
void free(void *ptr);

[[noreturn]] void abort(void);

#endif // stdlib_h_INCLUDED
