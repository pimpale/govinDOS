#ifndef math_h_INCLUDED
#define math_h_INCLUDED

// Type-generic min/max. Each argument is evaluated once.
#define max(a, b)                                                              \
  ({                                                                           \
    __auto_type _max_a = (a);                                                  \
    __auto_type _max_b = (b);                                                  \
    _max_a > _max_b ? _max_a : _max_b;                                         \
  })

#define min(a, b)                                                              \
  ({                                                                           \
    __auto_type _min_a = (a);                                                  \
    __auto_type _min_b = (b);                                                  \
    _min_a < _min_b ? _min_a : _min_b;                                         \
  })

#endif // math_h_INCLUDED
