%include 'if.mac'
%include 'ccall.mac'
%include 'vga_text.mac'

; get vga color from foreground (arg0) and background (arg1)
_vga_color: proc
  mov eax, [arg(0)] ; fg
  mov ecx, [arg(1)] ; bg
  shl ecx, 4 
  or  eax, ecx ; fg | bg << 4
endproc

; create vga entry from character (arg0) and vga_color (arg1)
_vga_entry: proc
  mov eax, [arg(0)] ; character
  mov ecx, [arg(1)] ; vga_color
  shl ecx, 8 ; color << 8
  or eax, ecx ; character | color
endproc

; write using character (arg0), with fg color (arg1), bg color (arg2) to buffer at x (arg3), and y (arg4)
_vga_putc: proc
  ; ensure its within bounds first
  if [arg(3)], L-THAN, VGA_XSIZE
    if [arg(4)], L-THAN, VGA_YSIZE
      ccall _vga_color, [arg(1)], [arg(2)] ; call vga_color with fg and bg
      push eax  ; make arg1
      push dword [arg(0)] 
      call _vga_entry ; call vga_entry with the new vga color and the text
      mov ecx, eax ; save result 
      mov eax, [arg(4)] ; y
      mov edx, VGA_XSIZE ; multiply by xsize
      mul edx
      add eax, [arg(3)] ; add x
      add eax, eax ; double it, because this is gonna be the byte offset, and we're dealing with 16bits
      add eax, VGA_BUFFER_LOC ; eax points to the location of this vga in the buffer 
      mov [eax], ecx ; set the value
    endif
  endif
endproc
 
