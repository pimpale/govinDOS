[BITS 64]

%include "call.mac"

[GLOBAL kinit]
[GLOBAL kmain]

[SECTION .text]

; this isn't actually a function, but it does call the real kmain
kinit:
  call kmain

kmain: proc
  mov rax, message
  mov rbx, 5
  call vga_print

  mov rax, 'X'
  out 0xe9, rax
  hlt
endproc


[SECTION .data]
message: db "hello"


