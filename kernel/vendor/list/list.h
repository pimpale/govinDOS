// Generic doubly-linked-list template. Include with LIST_DTYPE set to a
// typedef'd element type:
//
//   #define LIST_DTYPE thread_ptr
//   #include <list/list.h>
//
// Generates:
//   typedef struct list_thread_ptr list_thread_ptr;
//   void list_thread_ptr_new(list_thread_ptr **);
//   void list_thread_ptr_push_back(list_thread_ptr *, const thread_ptr *src);
//   ... etc
//
// LIST_DTYPE must be a single identifier usable in token pasting (typedef
// the type if needed, e.g. `typedef struct thread *thread_ptr;`). The
// element type must be complete at include time.
//
// This file has no header guard: include it once per LIST_DTYPE. It does
// not #undef LIST_DTYPE — the caller is expected to either leave it for
// list_impl.h or #undef it before the next instantiation.

#ifndef LIST_DTYPE
#error "LIST_DTYPE must be defined before including list.h"
#endif

#include <stddef.h>
#include <stdint.h>

#define LIST_PASTE_(a, b) a##b
#define LIST_PASTE(a, b) LIST_PASTE_(a, b)
#define LIST_T LIST_PASTE(list_, LIST_DTYPE)
#define LIST_FN(suffix) LIST_PASTE(LIST_T, suffix)

typedef struct LIST_T LIST_T;

void LIST_FN(_new)(LIST_T **list);
void LIST_FN(_delete)(LIST_T **list);

void LIST_FN(_push_back) (LIST_T *list, const LIST_DTYPE *src);
void LIST_FN(_push_front)(LIST_T *list, const LIST_DTYPE *src);

// Pop variants assert non-empty; caller checks _len() first.
void LIST_FN(_pop_front)(LIST_T *list, LIST_DTYPE *dest);
void LIST_FN(_pop_back) (LIST_T *list, LIST_DTYPE *dest);

uint32_t LIST_FN(_len)(const LIST_T *list);

#undef LIST_T
#undef LIST_FN
#undef LIST_PASTE
#undef LIST_PASTE_
