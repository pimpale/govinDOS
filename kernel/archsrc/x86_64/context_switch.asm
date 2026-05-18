[BITS 64]
default rel

[GLOBAL switch_context]
[GLOBAL thread_bootstrap]
[EXTERN thread_trampoline]

[SECTION .text]

; void switch_context(uint64_t *old_sp_out, uint64_t new_sp);
;
; Win64 ABI (we build with -target x86_64-unknown-windows):
;   rcx = old_sp_out
;   rdx = new_sp
;
; Saves Win64 callee-saved GPRs + RFLAGS on the outgoing stack, writes the
; resulting RSP into *old_sp_out, loads the incoming stack, restores the
; saved set, and rets to whatever return address is at its top.
;
; Note: xmm6-xmm15 are also callee-saved under Win64 but the kernel builds
; with -mgeneral-regs-only and never touches them, so we don't bother.
switch_context:
    push    rbp
    push    rbx
    push    rdi
    push    rsi
    push    r12
    push    r13
    push    r14
    push    r15
    pushfq

    mov     [rcx], rsp          ; *old_sp_out = rsp
    mov     rsp, rdx             ; switch to the new stack

    popfq
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rsi                  ; bootstrap-frame payload: arg
    pop     rdi                  ; bootstrap-frame payload: entry
    pop     rbx
    pop     rbp
    ret

; ----------------------------------------------------------------------------
; thread_bootstrap: ret target of a forged stack frame for a fresh thread.
;
; arch_thread_init_kernel sets up the stack so the switch_context restore
; lands here with:
;     rdi = entry function pointer
;     rsi = arg
;
; This shim translates Sys-V style argument regs into Win64 (rcx, rdx),
; allocates the 32-byte shadow space + an 8-byte slot for 16-byte SP
; alignment at call entry, then tail-calls thread_trampoline. The
; trampoline never returns (it calls thread_exit), so the halt loop is
; just defensive.
; ----------------------------------------------------------------------------
thread_bootstrap:
    mov     rcx, rdi             ; entry
    mov     rdx, rsi             ; arg
    sub     rsp, 40              ; 32 shadow + 8 align (call adds 8 -> 16-aligned in callee)
    call    thread_trampoline
.spin:
    hlt
    jmp     .spin
