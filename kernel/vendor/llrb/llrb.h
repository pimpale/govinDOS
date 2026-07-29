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
// instantiation additionally defines LLRB_COMPARE and may override the
// tree/node allocation hooks; see llrb_impl.h.
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

// Publicly complete so an allocator template can be instantiated for the
// generated node type. Callers must still treat these fields as private; the
// only supported direct-node operations are _extract and _insert_node.
struct LLRB_NODE_T {
  LLRB_KEY key;
  LLRB_VALUE value;
  LLRB_NODE_T *left;
  LLRB_NODE_T *right;
  LLRB_NODE_T *parent;
  bool red;
};

// Iterators are allocation-free. Mutating the tree invalidates all active
// iterators; otherwise each call advances in ascending key order.
typedef struct LLRB_ITER_T {
  const LLRB_NODE_T *node;
} LLRB_ITER_T;

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
bool LLRB_FN(_get_ref)(LLRB_T *tree, const LLRB_KEY *key,
                       LLRB_VALUE **value);
// Greatest entry whose key is <= *key.
bool LLRB_FN(_floor)(const LLRB_T *tree, const LLRB_KEY *key,
                     LLRB_KEY *found_key, LLRB_VALUE *value);
size_t LLRB_FN(_len)(const LLRB_T *tree);

void LLRB_FN(_iter_begin)(const LLRB_T *tree, LLRB_ITER_T *iter);
bool LLRB_FN(_iter_next)(LLRB_ITER_T *iter, LLRB_KEY *key,
                         LLRB_VALUE *value);
bool LLRB_FN(_iter_next_ref)(LLRB_ITER_T *iter, LLRB_KEY *key,
                             const LLRB_VALUE **value);

// Position the iterator at the first node with key >= *key (the lower
// bound), so a subsequent _iter_next starts there.
void LLRB_FN(_iter_lower_bound)(const LLRB_T *tree, const LLRB_KEY *key,
                                LLRB_ITER_T *iter);

// Allocation-free relink pair. _extract removes *key like _remove but
// hands back the unlinked node instead of freeing it; _insert_node
// inserts key/value reusing that node's storage (no allocation, so it
// cannot fail with OOM — false means duplicate key, and the node stays
// the caller's). Together they move an entry between two same-typed
// trees with strictly pointer work.
bool LLRB_FN(_extract)(LLRB_T *tree, const LLRB_KEY *key,
                       LLRB_VALUE *old_value, LLRB_NODE_T **node_out);
bool LLRB_FN(_insert_node)(LLRB_T *tree, const LLRB_KEY *key,
                           const LLRB_VALUE *value, LLRB_NODE_T *node);

// Intended for tests and diagnostics: checks ordering, parent links, left
// leaning, red adjacency, black height, and the recorded node count.
bool LLRB_FN(_valid)(const LLRB_T *tree);

#undef LLRB_T
#undef LLRB_NODE_T
#undef LLRB_ITER_T
#undef LLRB_FN
#undef LLRB_PASTE
#undef LLRB_PASTE_
