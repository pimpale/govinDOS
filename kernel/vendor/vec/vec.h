// Generic dynamic-array template. Include with VEC_DTYPE set to a typedef'd
// element type:
//
//   #define VEC_DTYPE process
//   #include <vec/vec.h>
//
// Generates:
//   typedef struct vec_process vec_process;
//   void vec_process_new(vec_process **);
//   void vec_process_push(vec_process *, const process *src);
//   ... etc
//
// VEC_DTYPE must be a single identifier usable in token pasting (typedef the
// struct if needed, e.g. `typedef struct process process;`). The element type
// must be complete at include time (include its header before this one).
//
// This file has no header guard: include it once per VEC_DTYPE. It does not
// #undef VEC_DTYPE — the caller is expected to either leave it for vec_impl.h
// or #undef it before the next instantiation.

#ifndef VEC_DTYPE
#error "VEC_DTYPE must be defined before including vec.h"
#endif

#include <stddef.h>
#include <stdint.h>

#define VEC_PASTE_(a, b) a##b
#define VEC_PASTE(a, b) VEC_PASTE_(a, b)
#define VEC_T VEC_PASTE(vec_, VEC_DTYPE)
#define VEC_FN(suffix) VEC_PASTE(VEC_T, suffix)

typedef struct VEC_T VEC_T;

void VEC_FN(_new)(VEC_T **vec);
void VEC_FN(_delete)(VEC_T **vec);

void VEC_FN(_push)(VEC_T *vec, const VEC_DTYPE *src);
void VEC_FN(_pop)(VEC_T *vec, VEC_DTYPE *dest);

void VEC_FN(_get)(const VEC_T *vec, uint32_t i, VEC_DTYPE *dest);

// swaps i with the last element and then pops, deleting the data
void VEC_FN(_swap_and_pop)(VEC_T *vec, uint32_t i);

void VEC_FN(_clear)(VEC_T *vec);

uint32_t VEC_FN(_len)(const VEC_T *vec);

#undef VEC_T
#undef VEC_FN
#undef VEC_PASTE
#undef VEC_PASTE_
