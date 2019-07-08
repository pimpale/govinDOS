[BITS 64]

%include "gdt.mac"

[SECTION .data]

; From https://wiki.osdev.org/Setting_Up_Long_Mode
; This is the early gdt that will be replaced later
[GLOBAL gdt]
[GLOBAL gdt.null]
[GLOBAL gdt.code]
[GLOBAL gdt.data]
[GLOBAL gdt.pointer]

gdt:                             ; Global Descriptor Table (64-bit).
  .null: equ $ - gdt             ; The null descriptor.
    dw 0xFFFF                    ; Limit (low).
    dw 0                         ; Base (low).
    db 0                         ; Base (middle)
    db 0                         ; Access.
    db 1                         ; Granularity.
    db 0                         ; Base (high).
  .code: equ $ - gdt             ; The code descriptor.
    dw 0                         ; Limit (low).
    dw 0                         ; Base (low).
    db 0                         ; Base (middle)
    db 10011010b                 ; Access (exec/read).
    db 10101111b                 ; Granularity,  bits flag, limit19:16.
    db 0                         ; Base (high).
  .data: equ $ - gdt             ; The data descriptor.
    dw 0                         ; Limit (low).
    dw 0                         ; Base (low).
    db 0                         ; Base (middle)
    db 10010010b                 ; Access (read/write).
    db 00000000b                 ; Granularity.
    db 0                         ; Base (high).
  .pointer:                      ; The gdt-pointer.
    dw $ - gdt - 1               ; Limit.
    dq gdt                       ; Base.
