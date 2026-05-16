#include "impl_stack_protector.h"

#include "debug.h"

// The MSVC-ABI stack protector references a global `__security_cookie` that
// the compiler stashes in each function's frame on entry and validates on
// exit via `__security_check_cookie`. We are freestanding with no CRT, so we
// provide both ourselves. The value is a fixed sentinel; randomizing it would
// require an entropy source we do not have this early in boot.
uintptr_t __security_cookie = (uintptr_t)0x00002B992DDFA232;

void __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie) {
        fatal("stack smashing detected: __security_cookie mismatch");
    }
}

// The MSVC x64 ABI inserts a call to `__chkstk` at the top of any function
// whose frame is >= one page (4096 bytes), passing the allocation size in
// RAX. Its job is to touch every page that will be subtracted off RSP so
// that guard pages below the stack trip a fault before the prologue actually
// reserves them. We don't have guard pages yet (kernel stacks will come from
// the buddy allocator later, with an unmapped guard page beneath each), so
// this is a no-op stub: preserve all registers and return. Once guard pages
// exist, replace the body with a loop that does `or QWORD [rsp - off], 0`
// at 4 KiB strides down to RAX.
__attribute__((naked))
void __chkstk(void) {
    __asm__ volatile("ret");
}
