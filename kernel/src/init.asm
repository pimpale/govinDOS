[BITS 64]

%include "call.mac"
%include "debug.mac"
%include "log.mac"

[EXTERN log_init]
[EXTERN log_write]
[EXTERN debug_write]


[SECTION .text]

; Not actual method, just starting point for 64 bit kernel
[GLOBAL kinit]
kinit:
  call kmain
  hlt

[GLOBAL kmain]
kmain: proc
  call log_init
  mov rax, 26
  mov rbx, message
  call log_write

  mov rax, 26
  mov rbx, message
  call debug_write
endproc


[SECTION .data]
message: db "abcdefghijklmnopqrstuv"


