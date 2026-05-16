#ifndef impl_stack_protector_h_INCLUDED
#define impl_stack_protector_h_INCLUDED

#include <stdint.h>

extern uintptr_t __security_cookie;

void __security_check_cookie(uintptr_t cookie);

#endif // impl_stack_protector_h_INCLUDED
