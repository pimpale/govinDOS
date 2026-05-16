[BITS 64]
default rel

[SECTION .text]
[EXTERN interrupt_handler]
[GLOBAL asm_load_idt]

; ----- ISR stubs -------------------------------------------------------------
;
; The CPU pushes an error code for vectors 8, 10, 11, 12, 13, 14, 17, 21, 29,
; 30; for every other vector it does not. To make the rest of the pipeline
; uniform, the non-err stubs push a fake 0 error code so both paths reach the
; common backend with the same stack shape.
;
; On entry to _isr_handler (low -> high addresses on the stack):
;     [rsp+0..95]  : saved GPRs (push order matches struct cpu_state)
;     [rsp+96]     : saved r9         (pushed by stub)
;     [rsp+104]    : saved r8         (pushed by stub)
;     [rsp+112]    : saved rdx        (pushed by stub)
;     [rsp+120]    : error code       (real, or fake 0 from stub)
;     [rsp+128]    : RIP              (start of IRET frame)
;     [rsp+136..]  : CS, RFLAGS, RSP, SS

%macro isr_stub_noerr 1
isr%1:
        push qword 0          ; fake error code
        push rdx
        push r8
        push r9
        mov  rdx, %1
        mov  r8,  [rsp+24]    ; error code (fake 0)
        mov  r9,  [rsp+32]    ; RIP
        jmp  _isr_handler
%endmacro

%macro isr_stub_err 1
isr%1:
        ; CPU has already pushed the error code at [rsp+0]
        push rdx
        push r8
        push r9
        mov  rdx, %1
        mov  r8,  [rsp+24]    ; real error code
        mov  r9,  [rsp+32]    ; RIP
        jmp  _isr_handler
%endmacro

%assign i 0
%rep 256
    %if (i = 8) || (i = 10) || (i = 11) || (i = 12) || (i = 13) || (i = 14) || (i = 17) || (i = 21) || (i = 29) || (i = 30)
        isr_stub_err i
    %else
        isr_stub_noerr i
    %endif
    %assign i i + 1
%endrep

; ----- IDT loader ------------------------------------------------------------

asm_load_idt:
        push rax
        mov rdx, idt
%assign i 0
%rep 256
        mov  rax, isr%+i
        mov  [rdx + 16 * i], ax
        shr  rax, 16
        mov  [rdx + 16 * i + 6], ax
        shr  rax, 16
        mov  [rdx + 16 * i + 8], eax
%assign i i + 1
%endrep
        lidt [idt_desc]
        pop  rax
        ret
.end:

; ----- Common backend --------------------------------------------------------

_isr_handler:
        push r15
        push r14
        push r13
        push r12
        push r11
        push r10
        push rdi
        push rsi
        push rbp
        push rcx
        push rbx
        push rax

        mov  rcx, rsp     ; 1st arg: pointer to cpu_state
        mov  rbx, rsp     ; rbx is callee-saved; stash rsp here
        and  rsp, -16     ; 16-byte align before call
        sub  rsp, 32      ; MS x64 shadow space

        call interrupt_handler

        mov  rsp, rbx     ; restore stack

        pop  rax
        pop  rbx
        pop  rcx
        pop  rbp
        pop  rsi
        pop  rdi
        pop  r10
        pop  r11
        pop  r12
        pop  r13
        pop  r14
        pop  r15

        pop  r9
        pop  r8
        pop  rdx
        add  rsp, 8       ; discard error code (real or fake)
        iretq

[SECTION .data]

idt:
%assign i 0
%rep 256
        dw 0xdead     ; isr 0..15
        dw 8h         ; kernel cs
        db 0          ; ist (0 disable)
%if i = 80h
        db 11101110b  ; attributes (usermode)
%else
        db 10001110b  ; attributes (kernelmode)
%endif
        dw 0xbeef     ; isr 16..31
        dd 0xcafebabe ; isr 32..63
        dd 0          ; reserved
%assign i i + 1
%endrep
idt_end:

idt_desc:
        dw (idt_end - idt)
        dq (idt)
