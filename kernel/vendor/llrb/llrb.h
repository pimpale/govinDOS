// Generic owning left-leaning red-black tree template. Include with a name,
// key type, and value type:
//
//   #define LLRB_NAME pid_process
//   #define LLRB_KEY uint64_t
//   #define LLRB_VALUE process_ptr
//   #include <llrb/llrb.h>
//
// This declares llrb_pid_process and its operations. Keys and values are
// copied into independently allocated nodes. The implementation
// instantiation additionally defines LLRB_COMPARE and may override
// LLRB_MALLOC / LLRB_FREE; see llrb_impl.h.
//
// LLRB_NAME must be a single identifier usable in token pasting. LLRB_KEY
// and LLRB_VALUE must be complete types. This file intentionally has no
// header guard so multiple differently named trees can be declared.

#ifndef LLRB_NAME
#error "LLRB_NAME must be defined before including llrb.h"
#endif
#ifndef LLRB_KEY
#error "LLRB_KEY must be defined before including llrb.h"
#endif
#ifndef LLRB_VALUE
#error "LLRB_VALUE must be defined before including llrb.h"
#endif

#include <stdbool.h>
#include <stddef.h>

#define LLRB_PASTE_(a, b) a##b
#define LLRB_PASTE(a, b) LLRB_PASTE_(a, b)
#define LLRB_T LLRB_PASTE(llrb_, LLRB_NAME)
#define LLRB_NODE_T LLRB_PASTE(LLRB_T, _node)
#define LLRB_ITER_T LLRB_PASTE(LLRB_T, _iter)
#define LLRB_FN(suffix) LLRB_PASTE(LLRB_T, suffix)

typedef struct LLRB_T LLRB_T;
typedef struct LLRB_NODE_T LLRB_NODE_T;

// Iterators are allocation-free. Mutating the tree invalidates all active
// iterators; otherwise each call advances in ascending key order.
typedef struct LLRB_ITER_T {
  const LLRB_NODE_T *node;
} LLRB_ITER_T;

// The allocation hooks are used for both the small tree object and nodes.
// False means allocation failure. _insert also returns false for a duplicate
// key and leaves the existing value unchanged.
bool LLRB_FN(_new)(LLRB_T **tree);
void LLRB_FN(_delete)(LLRB_T **tree);
void LLRB_FN(_clear)(LLRB_T *tree);

bool LLRB_FN(_insert)(LLRB_T *tree, const LLRB_KEY *key,
                      const LLRB_VALUE *value);
bool LLRB_FN(_remove)(LLRB_T *tree, const LLRB_KEY *key,
                      LLRB_VALUE *old_value);
bool LLRB_FN(_get)(const LLRB_T *tree, const LLRB_KEY *key,
                   LLRB_VALUE *value);
size_t LLRB_FN(_len)(const LLRB_T *tree);

void LLRB_FN(_iter_begin)(const LLRB_T *tree, LLRB_ITER_T *iter);
bool LLRB_FN(_iter_next)(LLRB_ITER_T *iter, LLRB_KEY *key,
                         LLRB_VALUE *value);

// Intended for tests and diagnostics: checks ordering, parent links, left
// leaning, red adjacency, black height, and the recorded node count.
bool LLRB_FN(_valid)(const LLRB_T *tree);

#undef LLRB_T
#undef LLRB_NODE_T
#undef LLRB_ITER_T
#undef LLRB_FN
#undef LLRB_PASTE
#undef LLRB_PASTE_
