#ifndef stdlib_h_INCLUDED
#define stdlib_h_INCLUDED

#include <stddef.h>

struct buddy_allocator_s;

// Install the buddy allocator backing malloc/calloc/realloc/free. Must be
// called once during kernel init before any allocation. The allocator must
// already be in the ready state.
void kstdlib_set_allocator(struct buddy_allocator_s *ba);

[[nodiscard]] void *malloc(size_t size);
[[nodiscard]] void *calloc(size_t nmemb, size_t size);
[[nodiscard]] void *realloc(void *ptr, size_t size);
void free(void *ptr);

#endif // stdlib_h_INCLUDED
