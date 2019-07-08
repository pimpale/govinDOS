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
  ; rax contains pointer to idt struct
  lea rax, [rax*IDT_SIZE+idt]
  ; r8 shall be our tmp register

  ; set offset 1
  mov r8, rbx
  and r8, 0xFFFF000000000000 ; bits 0-15
  mov WORD PTR [rax], r8
  ; set offset 2
  mov r8, rbx
  and r8, 0x0000FFFF00000000 ; bits 16-31
  mov WORD PTR [rax+6], r8

  ; and offset 3
  mov r8, rbx
  and r8, 0x00000000FFFFFFFF ; bits 32-63
  mov DWORD PTR [rax+8], r8

  ; set zeros
  mov r8, 0
  mov DWORD PTR [rax+12], r8

  ; set IST to zero for now 
  ; TODO when we do task switching
  mov BYTE PTR [rax+4], r8

  ; begin work on the flags byte
  mov r8, 0 ; set the flags to zero

  or r8, IDT_GATE_PRESENT << 7 ; set present bit

  shl rcx, 5 ; align the dpl
  or r8, rcx ; make dpl flags

  or r8, rdx ; set type (arg3)

  mov BYTE PTR [rax+5], r8
endproc





[SECTION .data]

idt.pointer:
  dw IDT_SIZE*IDT_MAX_COUNT
  dq idt

[SECTION .bss]
idt:
  resb IDT_SIZE*IDT_MAX_COUNT
