[BITS 64]

; constants
%include "call.mac"
%include "vga.mac"

[SECTION .text] 

; get vga color from foreground (arg0) and background (arg1)
; arg0: foreground color, as defined in vga.asm
; arg1: background color, as defined in vga.asm
vga_color: proc
  shl rbx, 4 
  or  rax, rbx ; fg | bg << 4
endproc

; create vga entry from character (arg0) and vga_color (arg1)
; arg0: 8 bit character
; arg1: vga_color created by vga_color32
vga_entry32: proc
  shl rbx, 8 ; color << 8
  or rax, rbx ; character | color
endproc

; This clears the screen with space characters with the given color arg0
; arg0: the color to clear the screen with
vga_clear_screen: proc

  ; get vga entry using provided color
  ; move color to 2nd arg
  mov rbx, rax
  mov rax, ' ' ; Char to clear screen with
  call vga_entry32
  ;  is now stored in rax

  mov rcx, VGA_BUFFER_LEN ; ecx is counter
  mov rdi, VGA_BUFFER_ADDR ; Location to write to
  cld ; Copy forwards

  ; Fill in buffer with value in ax 
  rep stosw
endproc

; Print vga chars to beginning, overwriting what is there, using pointer to null terminated string (arg0)
vga_print: proc
  mov edx, arg(0) ; Move the arg0 here
  mov ecx, 0 ; this register will serve as the counter
  .loop:
    mov al, [edx+ecx] ; Get a char from the string

    ; exit if null
    cmp al, 0
    je .end
    ; exit if we are going to exceed the limit 
    cmp ecx, VGA_BUFFER_LEN
    jge .end
    ; move char to buffer
    mov [VGA_BUFFER_ADDR+ecx*2], byte al 
    inc ecx
    jmp .loop
  .end:
endproc

