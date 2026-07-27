#ifndef slabs_h_INCLUDED
#define slabs_h_INCLUDED

// Initialize every generated fixed-type slab singleton. The buddy allocator
// and CPU-state table must already exist; normal SMP execution must not yet
// have begun.
void slabs_init(void);

#endif // slabs_h_INCLUDED
