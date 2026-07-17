[BITS 64]
default rel

[GLOBAL switch_context]
[GLOBAL uthread_resume]
[EXTERN uthread_resume_prepare]

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
    pop     rsi
    pop     rdi                  ; resume-frame payload: struct thread * (uthread_resume)
    pop     rbx
    pop     rbp
    ret

; ----------------------------------------------------------------------------
; uthread_resume: ret target of the forged frame in arch_thread.resume_stack.
;
; User threads have no kernel stack: their entire suspended state is the
; trap_frame saved in the TCB (arch_thread.uframe). The scheduler resumes
; them through the ordinary switch_context path; the forged frame carries
; rdi = struct thread *, and after the pops RSP sits at the top of the
; thread's tiny resume_stack — enough room for exactly one C call.
;
; uthread_resume_prepare does the non-asm bookkeeping (drops the scheduler's
; irq_disable depth without sti — iretq sets IF from the saved rflags —
; plus FSBASE/GSBASE/XSAVE restore) and returns &t->arch.uframe. We then
; point RSP at the frame and pop it in trap_frame order, mirroring the
; restore tail of _isr_handler in interrupts.asm.
; ----------------------------------------------------------------------------
uthread_resume:
    mov     rcx, rdi             ; arg0 = struct thread *
    sub     rsp, 40              ; 32 shadow + 8 align (call adds 8 -> 16-aligned in callee)
    call    uthread_resume_prepare
    mov     rsp, rax             ; rsp = &t->arch.uframe

    pop     rax
    pop     rbx
    pop     rcx
    pop     rbp
    pop     rsi
    pop     rdi
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15
    pop     r9
    pop     r8
    pop     rdx
    add     rsp, 8               ; skip the error-code slot
    iretq                        ; rip, cs, rflags, rsp, ss -> ring 3
