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
vga_entry: proc
  shl rbx, 8 ; color << 8
  or rax, rbx ; character | color
endproc

; This clears the screen with space characters with the given color arg0
; arg0: the vga_entry to clear the screen with
vga_fill_screen: proc
  ; vga_entry  is now stored in rax

  mov rcx, VGA_BUF_LEN  ; ecx is counter
  mov rdi, VGA_BUF ; Location to write to
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
  ; ensure there are no out of bound mem writes
  cmp rbx, VGA_XSIZE
  jae .end
  cmp rcx, VGA_YSIZE
  jae .end

  ; preserve vga char and move xsize to accumulator
  mov rsi, rax
  mov rax, rbx

  ; find buffer index to write to
  mul rcx
  add rax, rbx

  ; rax now contains the index to write to
  ; write to location
  mov [VGA_BUF+rax*2], word si
  .end:
endproc

