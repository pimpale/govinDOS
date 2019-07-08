[BITS 64]

%include "call.mac"
%include "interrupt.mac"

; sets a gate handling the arg0'th idt entry with the interrupt handler arg1
; arg0 the entry number
; arg1 the address of the handler
; arg2 the DPL, protects hardware and CPU interrupts from being called out of userspace.
; arg3 the type, as defined in interrupt.mac
; no returns
idt_set_gate_addr: proc
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
  
endproc

[SECTION .data]

idt.pointer:
  dw IDT_SIZE*IDT_MAX_COUNT
  dq idt

[SECTION .bss]
idt:
  resb IDT_SIZE*IDT_MAX_COUNT
