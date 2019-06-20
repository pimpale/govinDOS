[BITS 64]

%include "call.mac"

[GLOBAL kinit]
[GLOBAL kmain]

[SECTION .text]

; this isn't actually a function, but it does call the real kmain
kinit:
  call kmain

kmain: proc
  hlt
endproc
