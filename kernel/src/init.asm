[BITS 64]

%include "call.mac"
%include "debug.mac"
%include "log.mac"
%include "vga.mac"

[EXTERN log_init]
[EXTERN log_write]
[EXTERN debug_write]
[EXTERN vga_write]


[SECTION .text]


; Not actual method, just starting point for 64 bit kernel
[GLOBAL init]
init:
  ; call cpu_init
  ; call page_init
  call main
  hlt

main: proc
  call log_init
  mov rax, 26
  mov rbx, message
  call log_write

  mov rax, 26
  mov rbx, message
  call debug_write

  mov rax, 26
  mov rbx, message
  mov rcx, 6
  mov rdx, 7
  mov rsi, VGA_COLOR_GREEN_FG | VGA_COLOR_BLACK_BG
  call vga_write
endproc


[SECTION .data]
message: db "abcdefghijklmnopqrstuv"


