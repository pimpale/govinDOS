%include "call.mac"
%include "debug.mac"
[BITS 64]
[SECTION .text]

; arg0 number of bytes to print
; arg1 pointer to string message
; returns nothing
[GLOBAL debug_write]
debug_write: proc
  mov rdx, DBG_SERIAL_PORT ; print to correct port
  mov rsi, rbx             ; move arg1 to source
  mov rcx, rax             ; print out fixed num of bytes
  cld                      ; copy forward
  rep outsb                ; output bytes
endproc
