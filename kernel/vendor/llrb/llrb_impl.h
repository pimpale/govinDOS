// Generic owning left-leaning red-black tree implementation. Include exactly
// once per LLRB_NAME from a dedicated .c file. Required configuration:
//
//   #define LLRB_NAME pid_process
//   #define LLRB_KEY uint64_t
//   #define LLRB_VALUE process_ptr
//   #define LLRB_COMPARE(a, b) (((*a) > (*b)) - ((*a) < (*b)))
//   #include <llrb/llrb_impl.h>
//
// LLRB_COMPARE receives pointers to keys and returns negative, zero, or
// positive. The declaration header for this LLRB_NAME must already have been
// included. An instantiation may also define:
//
//   #define LLRB_MALLOC(size) my_slab_malloc(size)
//   #define LLRB_FREE(ptr) my_slab_free(ptr)
//   #define LLRB_TREE_MALLOC(size) my_tree_malloc(size)
//   #define LLRB_TREE_FREE(ptr) my_tree_free(ptr)
//   #define LLRB_NODE_MALLOC(size) my_node_malloc(size)
//   #define LLRB_NODE_FREE(ptr) my_node_free(ptr)
//
// The split hooks default through LLRB_MALLOC/LLRB_FREE, which in turn default
// to standard malloc/free.

#ifndef LLRB_NAME
#error "LLRB_NAME must be defined before including llrb_impl.h"
#endif
#ifndef LLRB_KEY
#error "LLRB_KEY must be defined before including llrb_impl.h"
#endif
#ifndef LLRB_VALUE
#error "LLRB_VALUE must be defined before including llrb_impl.h"
#endif
#ifndef LLRB_COMPARE
#error "LLRB_COMPARE(a, b) must be defined before including llrb_impl.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#ifndef LLRB_MALLOC
#define LLRB_MALLOC(size) malloc(size)
#define LLRB_DEFAULT_MALLOC
#endif
#ifndef LLRB_FREE
#define LLRB_FREE(ptr) free(ptr)
#define LLRB_DEFAULT_FREE
#endif
#ifndef LLRB_TREE_MALLOC
#define LLRB_TREE_MALLOC(size) LLRB_MALLOC(size)
#define LLRB_DEFAULT_TREE_MALLOC
#endif
#ifndef LLRB_TREE_FREE
#define LLRB_TREE_FREE(ptr) LLRB_FREE(ptr)
#define LLRB_DEFAULT_TREE_FREE
#endif
#ifndef LLRB_NODE_MALLOC
#define LLRB_NODE_MALLOC(size) LLRB_MALLOC(size)
#define LLRB_DEFAULT_NODE_MALLOC
#endif
#ifndef LLRB_NODE_FREE
#define LLRB_NODE_FREE(ptr) LLRB_FREE(ptr)
#define LLRB_DEFAULT_NODE_FREE
#endif

#define LLRB_PASTE_(a, b) a##b
#define LLRB_PASTE(a, b) LLRB_PASTE_(a, b)
#define LLRB_T LLRB_PASTE(llrb_, LLRB_NAME)
#define LLRB_NODE_T LLRB_PASTE(LLRB_T, _node)
#define LLRB_ITER_T LLRB_PASTE(LLRB_T, _iter)
#define LLRB_FN(suffix) LLRB_PASTE(LLRB_T, suffix)
#define LLRB_LOCAL(suffix) LLRB_PASTE(LLRB_FN(_impl_), suffix)

struct LLRB_T {
  LLRB_NODE_T *root;
  size_t len;
};

static bool LLRB_LOCAL(is_red)(const LLRB_NODE_T *n) {
  return n != NULL && n->red;
}

static LLRB_NODE_T *LLRB_LOCAL(rotate_left)(LLRB_NODE_T *h) {
  LLRB_NODE_T *x = h->right;
  h->right = x->left;
  if (h->right != NULL)
    h->right->parent = h;
  x->left = h;
  x->parent = h->parent;
  h->parent = x;
  x->red = h->red;
  h->red = true;
  return x;
}

static LLRB_NODE_T *LLRB_LOCAL(rotate_right)(LLRB_NODE_T *h) {
  LLRB_NODE_T *x = h->left;
  h->left = x->right;
  if (h->left != NULL)
    h->left->parent = h;
  x->right = h;
  x->parent = h->parent;
  h->parent = x;
  x->red = h->red;
  h->red = true;
  return x;
}

static void LLRB_LOCAL(flip_colors)(LLRB_NODE_T *h) {
  h->red = !h->red;
  h->left->red = !h->left->red;
  h->right->red = !h->right->red;
}

static LLRB_NODE_T *LLRB_LOCAL(fix_up)(LLRB_NODE_T *h) {
  if (LLRB_LOCAL(is_red)(h->right) && !LLRB_LOCAL(is_red)(h->left))
    h = LLRB_LOCAL(rotate_left)(h);
  if (LLRB_LOCAL(is_red)(h->left) && LLRB_LOCAL(is_red)(h->left->left))
    h = LLRB_LOCAL(rotate_right)(h);
  if (LLRB_LOCAL(is_red)(h->left) && LLRB_LOCAL(is_red)(h->right))
    LLRB_LOCAL(flip_colors)(h);
  return h;
}

// `spare`, when non-NULL, supplies the new node's storage instead of
// LLRB_NODE_MALLOC — the _insert_node path, which therefore cannot fail with
// OOM (result -1).
static LLRB_NODE_T *LLRB_LOCAL(insert)(LLRB_NODE_T *h, LLRB_NODE_T *parent,
                                       const LLRB_KEY *key,
                                       const LLRB_VALUE *value,
                                       LLRB_NODE_T *spare, int *result) {
  if (h == NULL) {
    LLRB_NODE_T *n =
        spare != NULL ? spare : LLRB_NODE_MALLOC(sizeof(*n));
    if (n == NULL) {
      *result = -1;
      return NULL;
    }
    n->key = *key;
    n->value = *value;
    n->left = NULL;
    n->right = NULL;
    n->parent = parent;
    n->red = true;
    *result = 1;
    return n;
  }

  int cmp = LLRB_COMPARE(key, &h->key);
  if (cmp < 0) {
    LLRB_NODE_T *left =
        LLRB_LOCAL(insert)(h->left, h, key, value, spare, result);
    if (*result < 0)
      return h;
    h->left = left;
  } else if (cmp > 0) {
    LLRB_NODE_T *right =
        LLRB_LOCAL(insert)(h->right, h, key, value, spare, result);
    if (*result < 0)
      return h;
    h->right = right;
  } else {
    *result = 0;
    return h;
  }
  return LLRB_LOCAL(fix_up)(h);
}

static LLRB_NODE_T *LLRB_LOCAL(move_red_left)(LLRB_NODE_T *h) {
  LLRB_LOCAL(flip_colors)(h);
  if (LLRB_LOCAL(is_red)(h->right->left)) {
    h->right = LLRB_LOCAL(rotate_right)(h->right);
    h->right->parent = h;
    h = LLRB_LOCAL(rotate_left)(h);
    LLRB_LOCAL(flip_colors)(h);
  }
  return h;
}

static LLRB_NODE_T *LLRB_LOCAL(move_red_right)(LLRB_NODE_T *h) {
  LLRB_LOCAL(flip_colors)(h);
  if (LLRB_LOCAL(is_red)(h->left->left)) {
    h = LLRB_LOCAL(rotate_right)(h);
    LLRB_LOCAL(flip_colors)(h);
  }
  return h;
}

static LLRB_NODE_T *LLRB_LOCAL(min)(LLRB_NODE_T *h) {
  while (h != NULL && h->left != NULL)
    h = h->left;
  return h;
}

static const LLRB_NODE_T *LLRB_LOCAL(cmin)(const LLRB_NODE_T *h) {
  while (h != NULL && h->left != NULL)
    h = h->left;
  return h;
}

// `keep`, when non-NULL, receives the exact matched node. Node addresses
// remain stable from insertion until removal: the two-child case splices
// the successor node into place instead of copying its payload.
static LLRB_NODE_T *LLRB_LOCAL(delete_min)(LLRB_NODE_T *h,
                                           LLRB_NODE_T **keep) {
  if (h->left == NULL) {
    if (keep != NULL)
      *keep = h;
    else
      LLRB_NODE_FREE(h);
    return NULL;
  }
  if (!LLRB_LOCAL(is_red)(h->left) &&
      !LLRB_LOCAL(is_red)(h->left->left))
    h = LLRB_LOCAL(move_red_left)(h);
  h->left = LLRB_LOCAL(delete_min)(h->left, keep);
  if (h->left != NULL)
    h->left->parent = h;
  return LLRB_LOCAL(fix_up)(h);
}

static LLRB_NODE_T *LLRB_LOCAL(remove)(LLRB_NODE_T *h, const LLRB_KEY *key,
                                       LLRB_NODE_T **keep) {
  if (LLRB_COMPARE(key, &h->key) < 0) {
    if (!LLRB_LOCAL(is_red)(h->left) &&
        !LLRB_LOCAL(is_red)(h->left->left))
      h = LLRB_LOCAL(move_red_left)(h);
    h->left = LLRB_LOCAL(remove)(h->left, key, keep);
    if (h->left != NULL)
      h->left->parent = h;
  } else {
    if (LLRB_LOCAL(is_red)(h->left))
      h = LLRB_LOCAL(rotate_right)(h);
    if (LLRB_COMPARE(key, &h->key) == 0 && h->right == NULL) {
      if (keep != NULL)
        *keep = h;
      else
        LLRB_NODE_FREE(h);
      return NULL;
    }
    if (!LLRB_LOCAL(is_red)(h->right) &&
        !LLRB_LOCAL(is_red)(h->right->left))
      h = LLRB_LOCAL(move_red_right)(h);
    if (LLRB_COMPARE(key, &h->key) == 0) {
      LLRB_NODE_T *successor = NULL;
      LLRB_NODE_T *right =
          LLRB_LOCAL(delete_min)(h->right, &successor);
      successor->left = h->left;
      successor->right = right;
      successor->parent = h->parent;
      successor->red = h->red;
      if (successor->left != NULL)
        successor->left->parent = successor;
      if (successor->right != NULL)
        successor->right->parent = successor;
      if (keep != NULL)
        *keep = h;
      else
        LLRB_NODE_FREE(h);
      h = successor;
    } else {
      h->right = LLRB_LOCAL(remove)(h->right, key, keep);
    }
    if (h->right != NULL)
      h->right->parent = h;
  }
  return LLRB_LOCAL(fix_up)(h);
}

static void LLRB_LOCAL(clear)(LLRB_NODE_T *h) {
  if (h == NULL)
    return;
  LLRB_LOCAL(clear)(h->left);
  LLRB_LOCAL(clear)(h->right);
  LLRB_NODE_FREE(h);
}

bool LLRB_FN(_new)(LLRB_T **tree) {
  LLRB_T *t = LLRB_TREE_MALLOC(sizeof(*t));
  if (t == NULL) {
    *tree = NULL;
    return false;
  }
  t->root = NULL;
  t->len = 0;
  *tree = t;
  return true;
}

void LLRB_FN(_clear)(LLRB_T *tree) {
  LLRB_LOCAL(clear)(tree->root);
  tree->root = NULL;
  tree->len = 0;
}

void LLRB_FN(_delete)(LLRB_T **tree) {
  if (*tree == NULL)
    return;
  LLRB_FN(_clear)(*tree);
  LLRB_TREE_FREE(*tree);
  *tree = NULL;
}

bool LLRB_FN(_insert)(LLRB_T *tree, const LLRB_KEY *key,
                      const LLRB_VALUE *value) {
  int result = 0;
  tree->root =
      LLRB_LOCAL(insert)(tree->root, NULL, key, value, NULL, &result);
  if (tree->root != NULL) {
    tree->root->parent = NULL;
    tree->root->red = false;
  }
  if (result == 1)
    tree->len++;
  return result == 1;
}

bool LLRB_FN(_insert_node)(LLRB_T *tree, const LLRB_KEY *key,
                           const LLRB_VALUE *value, LLRB_NODE_T *node) {
  int result = 0;
  tree->root =
      LLRB_LOCAL(insert)(tree->root, NULL, key, value, node, &result);
  if (tree->root != NULL) {
    tree->root->parent = NULL;
    tree->root->red = false;
  }
  if (result == 1)
    tree->len++;
  return result == 1;
}

bool LLRB_FN(_get)(const LLRB_T *tree, const LLRB_KEY *key,
                   LLRB_VALUE *value) {
  const LLRB_NODE_T *n = tree->root;
  while (n != NULL) {
    int cmp = LLRB_COMPARE(key, &n->key);
    if (cmp < 0)
      n = n->left;
    else if (cmp > 0)
      n = n->right;
    else {
      if (value != NULL)
        *value = n->value;
      return true;
    }
  }
  return false;
}

bool LLRB_FN(_get_ref)(LLRB_T *tree, const LLRB_KEY *key,
                       LLRB_VALUE **value) {
  LLRB_NODE_T *n = tree->root;
  while (n != NULL) {
    int cmp = LLRB_COMPARE(key, &n->key);
    if (cmp < 0)
      n = n->left;
    else if (cmp > 0)
      n = n->right;
    else {
      if (value != NULL)
        *value = &n->value;
      return true;
    }
  }
  return false;
}

bool LLRB_FN(_floor)(const LLRB_T *tree, const LLRB_KEY *key,
                     LLRB_KEY *found_key, LLRB_VALUE *value) {
  const LLRB_NODE_T *n = tree->root;
  const LLRB_NODE_T *best = NULL;
  while (n != NULL) {
    int cmp = LLRB_COMPARE(key, &n->key);
    if (cmp < 0) {
      n = n->left;
    } else {
      best = n;
      if (cmp == 0)
        break;
      n = n->right;
    }
  }
  if (best == NULL)
    return false;
  if (found_key != NULL)
    *found_key = best->key;
  if (value != NULL)
    *value = best->value;
  return true;
}

static bool LLRB_LOCAL(remove_root)(LLRB_T *tree, const LLRB_KEY *key,
                                    LLRB_VALUE *old_value,
                                    LLRB_NODE_T **keep) {
  if (!LLRB_FN(_get)(tree, key, old_value))
    return false;
  if (!LLRB_LOCAL(is_red)(tree->root->left) &&
      !LLRB_LOCAL(is_red)(tree->root->right))
    tree->root->red = true;
  tree->root = LLRB_LOCAL(remove)(tree->root, key, keep);
  if (tree->root != NULL) {
    tree->root->parent = NULL;
    tree->root->red = false;
  }
  tree->len--;
  return true;
}

bool LLRB_FN(_remove)(LLRB_T *tree, const LLRB_KEY *key,
                      LLRB_VALUE *old_value) {
  return LLRB_LOCAL(remove_root)(tree, key, old_value, NULL);
}

bool LLRB_FN(_extract)(LLRB_T *tree, const LLRB_KEY *key,
                       LLRB_VALUE *old_value, LLRB_NODE_T **node_out) {
  LLRB_NODE_T *kept = NULL;
  if (!LLRB_LOCAL(remove_root)(tree, key, old_value, &kept))
    return false;
  kept->left = NULL;
  kept->right = NULL;
  kept->parent = NULL;
  *node_out = kept;
  return true;
}

size_t LLRB_FN(_len)(const LLRB_T *tree) { return tree->len; }

void LLRB_FN(_iter_begin)(const LLRB_T *tree, LLRB_ITER_T *iter) {
  iter->node = LLRB_LOCAL(cmin)(tree->root);
}

void LLRB_FN(_iter_lower_bound)(const LLRB_T *tree, const LLRB_KEY *key,
                                LLRB_ITER_T *iter) {
  const LLRB_NODE_T *n = tree->root;
  const LLRB_NODE_T *best = NULL;
  while (n != NULL) {
    int cmp = LLRB_COMPARE(key, &n->key);
    if (cmp <= 0) {
      best = n;
      if (cmp == 0)
        break;
      n = n->left;
    } else {
      n = n->right;
    }
  }
  iter->node = best;
}

bool LLRB_FN(_iter_next)(LLRB_ITER_T *iter, LLRB_KEY *key,
                         LLRB_VALUE *value) {
  const LLRB_NODE_T *n = iter->node;
  if (n == NULL)
    return false;
  if (key != NULL)
    *key = n->key;
  if (value != NULL)
    *value = n->value;

  if (n->right != NULL) {
    iter->node = LLRB_LOCAL(cmin)(n->right);
  } else {
    const LLRB_NODE_T *parent = n->parent;
    while (parent != NULL && n == parent->right) {
      n = parent;
      parent = parent->parent;
    }
    iter->node = parent;
  }
  return true;
}

bool LLRB_FN(_iter_next_ref)(LLRB_ITER_T *iter, LLRB_KEY *key,
                             const LLRB_VALUE **value) {
  const LLRB_NODE_T *n = iter->node;
  if (n == NULL)
    return false;
  if (key != NULL)
    *key = n->key;
  if (value != NULL)
    *value = &n->value;

  if (n->right != NULL) {
    iter->node = LLRB_LOCAL(cmin)(n->right);
  } else {
    const LLRB_NODE_T *parent = n->parent;
    while (parent != NULL && n == parent->right) {
      n = parent;
      parent = parent->parent;
    }
    iter->node = parent;
  }
  return true;
}

struct LLRB_LOCAL(validation) {
  bool valid;
  size_t count;
  size_t black_height;
};

static struct LLRB_LOCAL(validation)
LLRB_LOCAL(validate)(const LLRB_NODE_T *n, const LLRB_NODE_T *parent,
                     const LLRB_KEY *low, const LLRB_KEY *high) {
  if (n == NULL)
    return (struct LLRB_LOCAL(validation)){true, 0, 1};
  if (n->parent != parent || (low != NULL && LLRB_COMPARE(&n->key, low) <= 0) ||
      (high != NULL && LLRB_COMPARE(&n->key, high) >= 0) ||
      LLRB_LOCAL(is_red)(n->right) ||
      (LLRB_LOCAL(is_red)(n) && LLRB_LOCAL(is_red)(n->left)))
    return (struct LLRB_LOCAL(validation)){false, 0, 0};
  struct LLRB_LOCAL(validation) left =
      LLRB_LOCAL(validate)(n->left, n, low, &n->key);
  struct LLRB_LOCAL(validation) right =
      LLRB_LOCAL(validate)(n->right, n, &n->key, high);
  if (!left.valid || !right.valid || left.black_height != right.black_height)
    return (struct LLRB_LOCAL(validation)){false, 0, 0};
  return (struct LLRB_LOCAL(validation)){
      true, left.count + right.count + 1,
      left.black_height + (n->red ? 0 : 1)};
}

bool LLRB_FN(_valid)(const LLRB_T *tree) {
  if (tree->root != NULL && (tree->root->red || tree->root->parent != NULL))
    return false;
  struct LLRB_LOCAL(validation) v =
      LLRB_LOCAL(validate)(tree->root, NULL, NULL, NULL);
  return v.valid && v.count == tree->len;
}

#ifdef LLRB_DEFAULT_NODE_FREE
#undef LLRB_NODE_FREE
#undef LLRB_DEFAULT_NODE_FREE
#endif
#ifdef LLRB_DEFAULT_NODE_MALLOC
#undef LLRB_NODE_MALLOC
#undef LLRB_DEFAULT_NODE_MALLOC
#endif
#ifdef LLRB_DEFAULT_TREE_FREE
#undef LLRB_TREE_FREE
#undef LLRB_DEFAULT_TREE_FREE
#endif
#ifdef LLRB_DEFAULT_TREE_MALLOC
#undef LLRB_TREE_MALLOC
#undef LLRB_DEFAULT_TREE_MALLOC
#endif
#ifdef LLRB_DEFAULT_FREE
#undef LLRB_FREE
#undef LLRB_DEFAULT_FREE
#endif
#ifdef LLRB_DEFAULT_MALLOC
#undef LLRB_MALLOC
#undef LLRB_DEFAULT_MALLOC
#endif
#undef LLRB_T
#undef LLRB_NODE_T
#undef LLRB_ITER_T
#undef LLRB_FN
#undef LLRB_LOCAL
#undef LLRB_PASTE
#undef LLRB_PASTE_
