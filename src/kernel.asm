%include 'ccall.mac'
%include 'vga_text.mac'

global _kernel_main
_kernel_main: proc
  extern _vga_putc
  ccall _vga_putc, 65, VGA_COLOR_CYAN, VGA_COLOR_BLUE, 5, 5
endproc 

section .data
