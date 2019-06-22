[BITS 64]

; constants
%include "call.mac"
%include "vga.mac"

[SECTION .text] 

; get vga color from foreground (arg0) and background (arg1)
; arg0: foreground color, as defined in vga.mac
; arg1: background color, as defined in vga.mac
; ret0: vga_color, a combination of background and foreground
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
  ; vga_entry  is now stored in rax

  mov rcx, VGA_BUFFER_LEN ; ecx is counter
  mov rdi, VGA_BUFFER_ADDR ; Location to write to
  cld ; Copy forwards

  ; Fill in buffer with value in ax 
  rep stosw
endproc

; Places specified vga_char (arg0) at location x (arg1) and y (arg2)
; arg0: vga_char 
; arg1: x location (x values greater than VGA_XSIZE will be ignored)
; arg2: y location (y values greater than VGA_YSIZE will be ignored)
; no return
vga_putchar: proc

endproc

; Print vga chars to beginning, overwriting what is there, using pointer to string (arg0) and length of string
; arg0: pointer to string
; arg1: length of string
; returns error if 
vga_print: proc
  ; rax contains string pointer
  ; rbx contains the length of a string
  ; rcx contains the counter 
  
  mov rcx, 0 ; this register will serve as the counter
  .loop:
    mov al, [rax+rcx] ; Get a char from the string

    ; exit if we are going to exceed the string length
    cmp rcx, rbx
    jge .end

    ; exit if we are going to exceed the limit 
    cmp ecx, VGA_BUFFER_LEN
    jge .end
    ; move char to buffer
    mov [VGA_BUFFER_ADDR+ecx*2], byte al 
    inc ecx
    dec rbx
    jmp .loop
  .end:
endproc

