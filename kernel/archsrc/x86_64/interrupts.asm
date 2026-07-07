[BITS 64]
default rel

[SECTION .text]
[EXTERN interrupt_handler]
[EXTERN syscall_entry_from_user]
[GLOBAL interrupts_fill_idt]
[GLOBAL interrupts_load_idt]
[GLOBAL syscall_entry_stub]

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

; ----- IDT setup -------------------------------------------------------------
;
; The IDT table itself is shared by every CPU, so its 256 ISR pointers only
; need to be written once. asm_fill_idt does that work and is called from the
; BSP during bring-up. interrupts_load_idt is the per-CPU operation: it just points
; this CPU's IDTR at the (already populated) table.

interrupts_fill_idt:
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
        pop  rax
        ret

interrupts_load_idt:
        lidt [idt_desc]
        ret
.end:

; ----- SYSCALL entry ----------------------------------------------------------
;
; IA32_LSTAR points here (cpu_setup.c). On entry from ring 3:
;   rcx = user rip, r11 = user rflags (both saved by the CPU),
;   rsp = still the USER stack, IF cleared by IA32_FMASK.
; SYSCALL does not switch stacks, so the stub does: IA32_KERNEL_GS_BASE
; holds this CPU's syscall_anchor (cpu_state.h) — gs:[0] scratch slot,
; gs:[8] kernel RSP0 top (the same per-CPU stack interrupt entries use;
; safe because IRQs stay masked for the whole syscall).
;
; The stub forges the exact stack image _isr_handler builds — a struct
; trap_frame — so syscall_entry, frame-save parking, and the iretq resume
; path are shared with the interrupt path verbatim. Two ABI quirks vs the
; old int-gate path, both architectural to SYSCALL:
;   - the first argument arrives in r10 and is stored into the frame's
;     rcx slot (SYSCALL clobbered rcx with the return rip);
;   - user rcx/r11 are not preserved (they return as rip/rflags).
;
; User CS/SS are not read from anywhere: SYSCALL only enters from ring 3
; flat segments, so the frame gets the constants (0x2B/0x23, gdt.h).
syscall_entry_stub:
        swapgs
        mov  [gs:0], rsp      ; stash user rsp
        mov  rsp, [gs:8]      ; switch to this CPU's kernel stack
        push qword 0x23       ; ss  = GDT_SEL_USER_DS
        push qword [gs:0]     ; rsp (user)
        swapgs                ; gs done — restore user gs base
        push r11              ; rflags (CPU saved it here)
        push qword 0x2B       ; cs  = GDT_SEL_USER_CS64
        push rcx              ; rip (CPU saved it here)
        push qword 0          ; error code slot (keeps trap_frame shape)
        push rdx
        push r8
        push r9
        push r15
        push r14
        push r13
        push r12
        push r11
        push r10
        push rdi
        push rsi
        push rbp
        push r10              ; rcx slot <- r10: arg0 travels in r10
        push rbx
        push rax

        mov  rcx, rsp         ; 1st arg: pointer to trap frame
        mov  rbx, rsp         ; callee-saved stash (user rbx is in the frame)
        and  rsp, -16
        sub  rsp, 32          ; MS x64 shadow space
        call syscall_entry_from_user
        mov  rsp, rbx

        ; Restore user state from the frame (the handler may have written
        ; results into it) and return via SYSRET.
        pop  rax
        pop  rbx
        add  rsp, 8           ; rcx slot: rcx is about to be the return rip
        pop  rbp
        pop  rsi
        pop  rdi
        pop  r10
        add  rsp, 8           ; r11 slot: r11 is about to be rflags
        pop  r12
        pop  r13
        pop  r14
        pop  r15
        pop  r9
        pop  r8
        pop  rdx
        add  rsp, 8           ; error code
        pop  rcx              ; rip
        add  rsp, 8           ; cs (SYSRET loads it from IA32_STAR)
        pop  r11              ; rflags
        pop  rsp              ; user rsp — kernel stack abandoned (reused
                              ; fresh by the next entry, like a parked exit)
        o64 sysret

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

; IST slot assignments must match the C constants in gdt.h:
;   IST_DOUBLE_FAULT  = 1  (vector 0x08)
;   IST_NMI           = 2  (vector 0x02)
;   IST_PAGE_FAULT    = 3  (vector 0x0E)
;   IST_MACHINE_CHECK = 4  (vector 0x12)
; Other vectors run on the current stack (kernel) or RSP0 (from ring 3).
idt:
%assign i 0
%rep 256
        dw 0xdead     ; isr 0..15
        dw 8h         ; kernel cs
%if i = 2
        db 2          ; IST_NMI
%elif i = 8
        db 1          ; IST_DOUBLE_FAULT
%elif i = 14
        db 3          ; IST_PAGE_FAULT
%elif i = 18
        db 4          ; IST_MACHINE_CHECK
%else
        db 0          ; no IST
%endif
        db 10001110b  ; attributes (kernelmode; syscalls enter via SYSCALL,
                      ; so a user `int NN` on any vector just #GPs)
        dw 0xbeef     ; isr 16..31
        dd 0xcafebabe ; isr 32..63
        dd 0          ; reserved
%assign i i + 1
%endrep
idt_end:

idt_desc:
        dw (idt_end - idt)
        dq (idt)
