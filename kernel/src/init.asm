[BITS 64]

%include "call.mac"
%include "debug.mac"

[EXTERN dbg_serial_put]


[SECTION .text]

; Not actual method, just starting point for 64 bit kernel
[GLOBAL kinit]
kinit:
  call kmain
  hlt

[GLOBAL kmain]
kmain: proc
  mov rax, 1
  mov rbx, message
  call dbg_serial_put
endproc


[SECTION .data]
message: db "hello"


