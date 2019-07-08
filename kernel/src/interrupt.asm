[BITS 64]

%include "call.mac"
%include "interrupt.mac"

interrupt_default_handler: proc_interrupt
  

; sets a gate handling the arg0'th idt entry with the interrupt handler arg1
; arg0 the entry number
; arg1 the address of the handler
; arg2 the DPL, protects hardware and CPU interrupts from being called out of userspace.
; arg3 the type, as defined in interrupt.mac
; no returns
idt_set_gate: proc
  ; r8 contains pointer to idt struct
  lea r8, [rax*IDT_SIZE+idt]
  ; rax shall now be our tmp register

  ; set offset 1
  mov rax, rbx
  mov [r8], ax
  ; set offset 2
  mov rax, rbx
  shl rax, 16
  mov [r8+6], ax

  ; and offset 3
  mov rax, rbx
  shl rax, 32
  mov [r8+8], eax

  ; set zeros
  mov rax, 0
  mov [r8+12], eax

  ; set IST to zero for now
  ; TODO when we do task switching
  mov [r8+4], al

  ; begin work on the flags byte
  ; rax should already be zero

  or al, IDT_GATE_PRESENT << 7 ; set present bit

  shl rcx, 5 ; align the dpl
  or al, cl ; make dpl flags

  or al, dl ; set type (arg3)

  mov [r8+5], dl ; set flags
endproc


idt_init: proc
  ; we first initialize all idt entries
  ; the entry has the default interrupt handler
  ; it can only be called from the kernel
  ; it is an interupt
  mov rax, 0
  mov rbx, interrupt_default_handler
  mov rcx, IDT_GATE_DPL_0
  mov rdx, IDT_GATE_TYPE_INT32
  .initialidtloop:
    call idt_set_gate
    inc rax
    cmp rax, IDT_MAX_COUNT
    jb .initialidtloop
  
endproc

[SECTION .data]

idt.pointer:
  dw IDT_SIZE*IDT_MAX_COUNT
  dq idt

[SECTION .bss]
idt:
  resb IDT_SIZE*IDT_MAX_COUNT
