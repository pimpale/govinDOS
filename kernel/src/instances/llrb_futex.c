#include "futex.h"

static int futex_compare(const struct futex_key *a,
                         const struct futex_key *b) {
  if (a->address != b->address)
    return (a->address > b->address) - (a->address < b->address);
  return (a->seq > b->seq) - (a->seq < b->seq);
}

#define LLRB_NAME futex
#define LLRB_KEY struct futex_key
#define LLRB_VALUE thread_ptr
#define LLRB_COMPARE(a, b) futex_compare((a), (b))
#include <llrb/llrb_impl.h>
#undef LLRB_COMPARE
#undef LLRB_VALUE
#undef LLRB_KEY
#undef LLRB_NAME
