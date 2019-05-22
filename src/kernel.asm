%include 'ccall.mac'
%include 'vga_text.mac'

global _kernel_main
_kernel_main: proc
  extern _vga_text_init
  call _vga_text_init
endproc 

section .data
