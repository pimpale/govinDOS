#include <stdint.h>
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
// that the guard page below the stack faults *before* the prologue's
// `sub rsp, rax` skips over it — i.e. it turns a large stack-clash overflow
// into a clean #PF on the guard.
//
// Contract (matches LLVM compiler-rt's ___chkstk):
//   in:  RAX = allocation size in bytes
//   out: ALL GPRs preserved, including RAX — the caller does
//        `sub rsp, rax` immediately after the call, so trashing RAX
//        means the prologue reserves the wrong amount and the frame is
//        corrupt before the first instruction runs. (The previous version
//        of this stub clobbered RAX in the probe loop, which silently
//        broke any caller with a frame > 4 KiB.)
//
// Algorithm: from the caller's RSP, walk down at 4 KiB strides probing each
// page with `or dword [rcx], 0` (a non-destructive read-modify-write).
// After the last full-page step, probe the final partial-page tail at
// `caller_rsp - rax`. Any unmapped page along the way takes a #PF here,
// while the instruction pointer still belongs to the calling function —
// much more useful than the alternative (the prologue silently skipping
// the guard, and the fault happening on whatever local touched the new
// frame first).
//
// Save layout while running:
//   [rsp+ 0] = saved rax
//   [rsp+ 8] = saved rcx
//   [rsp+16] = return address
//   [rsp+24] = caller's RSP at the call site
__attribute__((naked))
void __chkstk(void) {
    __asm__ volatile(
        ".intel_syntax noprefix\n\t"
        "push rcx\n\t"
        "push rax\n\t"
        "lea  rcx, [rsp + 24]\n\t"        // rcx = caller's RSP
    "1:\n\t"
        "cmp  rax, 0x1000\n\t"
        "jbe  2f\n\t"
        "sub  rcx, 0x1000\n\t"
        "or   dword ptr [rcx], 0\n\t"
        "sub  rax, 0x1000\n\t"
        "jmp  1b\n\t"
    "2:\n\t"
        "sub  rcx, rax\n\t"
        "or   dword ptr [rcx], 0\n\t"     // final tail probe
        "pop  rax\n\t"                    // restore caller's allocation size
        "pop  rcx\n\t"
        "ret\n\t"
        ".att_syntax prefix\n\t"
    );
}
