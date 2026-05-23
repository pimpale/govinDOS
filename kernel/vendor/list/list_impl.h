// Generic doubly-linked-list template implementation. Include exactly once
// per LIST_DTYPE, from a dedicated .c file in kernel/src/instances/:
//
//   #define LIST_DTYPE thread_ptr
//   #include "thread.h"            // brings in `thread_ptr` typedef
//   #include <list/list_impl.h>
//
// This file pulls in list.h itself, so callers do not need to include it
// separately. No header guard: each .c file instantiates one LIST_DTYPE.

#ifndef LIST_DTYPE
#error "LIST_DTYPE must be defined before including list_impl.h"
#endif

#include <stdint.h>

#include <assert.h>
#include <stdlib.h>
#include <list/list.h>

#define LIST_PASTE_(a, b) a##b
#define LIST_PASTE(a, b) LIST_PASTE_(a, b)
#define LIST_T LIST_PASTE(list_, LIST_DTYPE)
#define LIST_NODE_T LIST_PASTE(LIST_T, _node)
#define LIST_FN(suffix) LIST_PASTE(LIST_T, suffix)

typedef struct LIST_NODE_T LIST_NODE_T;
struct LIST_NODE_T {
  LIST_DTYPE val;
  LIST_NODE_T *prev;
  LIST_NODE_T *next;
};

struct LIST_T {
  LIST_NODE_T *head;
  LIST_NODE_T *tail;
  uint32_t len;
};

void LIST_FN(_new)(LIST_T **pList) {
  LIST_T *list = malloc(sizeof(LIST_T));
  list->head = NULL;
  list->tail = NULL;
  list->len = 0;
  *pList = list;
}

void LIST_FN(_delete)(LIST_T **pList) {
  LIST_NODE_T *n = (*pList)->head;
  while (n != NULL) {
    LIST_NODE_T *next = n->next;
    free(n);
    n = next;
  }
  free(*pList);
  *pList = NULL;
}

void LIST_FN(_push_back)(LIST_T *list, const LIST_DTYPE *src) {
  LIST_NODE_T *n = malloc(sizeof(LIST_NODE_T));
  n->val = *src;
  n->prev = list->tail;
  n->next = NULL;
  if (list->tail != NULL) {
    list->tail->next = n;
  } else {
    list->head = n;
  }
  list->tail = n;
  list->len++;
}

void LIST_FN(_push_front)(LIST_T *list, const LIST_DTYPE *src) {
  LIST_NODE_T *n = malloc(sizeof(LIST_NODE_T));
  n->val = *src;
  n->prev = NULL;
  n->next = list->head;
  if (list->head != NULL) {
    list->head->prev = n;
  } else {
    list->tail = n;
  }
  list->head = n;
  list->len++;
}

void LIST_FN(_pop_front)(LIST_T *list, LIST_DTYPE *dest) {
  assert(list->len > 0);
  LIST_NODE_T *n = list->head;
  *dest = n->val;
  list->head = n->next;
  if (list->head != NULL) {
    list->head->prev = NULL;
  } else {
    list->tail = NULL;
  }
  free(n);
  list->len--;
}

void LIST_FN(_pop_back)(LIST_T *list, LIST_DTYPE *dest) {
  assert(list->len > 0);
  LIST_NODE_T *n = list->tail;
  *dest = n->val;
  list->tail = n->prev;
  if (list->tail != NULL) {
    list->tail->next = NULL;
  } else {
    list->head = NULL;
  }
  free(n);
  list->len--;
}

uint32_t LIST_FN(_len)(const LIST_T *list) { return list->len; }

#undef LIST_T
#undef LIST_NODE_T
#undef LIST_FN
#undef LIST_PASTE
#undef LIST_PASTE_
