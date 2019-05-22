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

; calculate offset using x (arg0) and y (arg1)
global _vga_text_offset
_vga_text_offset: proc
  mov eax, [arg(1)] ; y
  mov edx, VGA_XSIZE ; multiply by xsize
  mul edx
  add eax, [arg(0)] ; add x
endproc 

; write using vga_entry (arg0) to buffer at offset (arg1)
global _vga_put_entry
_vga_put_entry: proc
  mov eax, [arg(0)]
  add eax, eax ; double it to translate it into byte offset
  add eax, VGA_BUFFER_LOC ; add the location
  mov ecx, [arg(1)] ; load from args
  mov [eax], ecx ; set mem
endproc

; Initializes vga text terminal
global _vga_text_init
_vga_text_init: proc

  ; save ebx
  push ebx

  ; Get the text color
  ccall _vga_color, VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK
  ; Get the vga entry
  ccall _vga_entry, ' ', eax
  ; store in ebx
  mov ebx, eax
  
  ; loop through all values
  mov ecx, VGA_XSIZE ; x
  _vga_text_init_loop_x:
    dec ecx ; x--
    mov edx, VGA_YSIZE ; y
    _vga_text_init_loop_y:
      dec edx ; y--
      ; TODO bug where i need some more registers (stack begone)
      ccall _vga_text_offset, ecx, edx ; get offset
      ccall _vga_put_entry, ebx, eax
      cmp edx, 0
      jne _vga_text_init_loop_y
    cmp ecx, 0
    jne _vga_text_init_loop_x 

  pop ebx ; restore ebx
endproc

      

      
      
      

    

 
