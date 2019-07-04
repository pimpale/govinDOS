%include "call.mac"
%include "debug.mac"
[BITS 64]
[SECTION .text]

; Places debug message in serial port
; arg0 priority
; arg1 pointer to string message
; returns void
[GLOBAL dbg_serial_put]
dbg_serial_put: proc
  mov rdx, DBG_SERIAL_PORT ; print to correct port
  mov rsi, rbx             ; move arg1 to source
  mov rcx, DBG_MSG_SIZE    ; print out fixed num of bytes
  cld                      ; copy forward
  rep outsb                ; output bytes
endproc
