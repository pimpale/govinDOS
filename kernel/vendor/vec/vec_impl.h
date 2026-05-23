// Generic dynamic-array template implementation. Include exactly once per
// VEC_DTYPE, from a dedicated .c file in kernel/src/instances/:
//
//   #define VEC_DTYPE process
//   #include "thread.h"            // brings in `process` typedef
//   #include <vec/vec_impl.h>
//
// This file pulls in vec.h itself, so callers do not need to include it
// separately. No header guard: each .c file instantiates one VEC_DTYPE.

#ifndef VEC_DTYPE
#error "VEC_DTYPE must be defined before including vec_impl.h"
#endif

#include <stdint.h>

#include <assert.h>
#include <stdlib.h>
#include <vec/vec.h>

#define VEC_PASTE_(a, b) a##b
#define VEC_PASTE(a, b) VEC_PASTE_(a, b)
#define VEC_T VEC_PASTE(vec_, VEC_DTYPE)
#define VEC_FN(suffix) VEC_PASTE(VEC_T, suffix)

struct VEC_T {
  VEC_DTYPE *pData;
  uint32_t cap;
  uint32_t len;
};

void VEC_FN(_new)(VEC_T **pVec) {
  VEC_T *vec = malloc(sizeof(VEC_T));
  vec->len = 0;
  vec->cap = 16;
  vec->pData = malloc(vec->cap * sizeof(VEC_DTYPE));
  *pVec = vec;
}

void VEC_FN(_push)(VEC_T *vec, const VEC_DTYPE *src) {
  if (vec->len >= vec->cap) {
    vec->cap *= 2;
    vec->pData = realloc(vec->pData, vec->cap * sizeof(VEC_DTYPE));
  }
  vec->pData[vec->len] = *src;
  vec->len++;
}

void VEC_FN(_pop)(VEC_T *vec, VEC_DTYPE *dest) {
  assert(vec->len > 0);
  *dest = vec->pData[vec->len - 1];
  vec->len--;
}

void VEC_FN(_clear)(VEC_T *vec) { vec->len = 0; }

void VEC_FN(_get)(const VEC_T *vec, uint32_t i, VEC_DTYPE *dest) {
  assert(i < vec->len);
  *dest = vec->pData[i];
}

void VEC_FN(_swap_and_pop)(VEC_T *vec, uint32_t i) {
  assert(vec->len > 0);
  assert(i < vec->len);
  vec->pData[i] = vec->pData[vec->len - 1];
  vec->len--;
}

uint32_t VEC_FN(_len)(const VEC_T *vec) { return vec->len; }

void VEC_FN(_delete)(VEC_T **pVec) {
  free((*pVec)->pData);
  free(*pVec);
  *pVec = NULL;
}

#undef VEC_T
#undef VEC_FN
#undef VEC_PASTE
#undef VEC_PASTE_
