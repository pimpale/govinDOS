[BITS 64]

; constants
%include "call.mac"
%include "vga.mac"

[SECTION .text]

; This clears the screen with space characters with the given vga_entry arg0
; arg0: the vga_entry to clear the screen with
vga_fill_screen: proc
  ; vga_entry  is now stored in rax

  mov rcx, VGA_BUF_LEN  ; ecx is counter
  mov rdi, VGA_BUF ; Location to write to
  cld ; Copy forwards

  ; Fill in buffer with value in ax
  rep stosw
endproc

; struct  cursor
; u64  x
; u64  y
; enum color

; Places specified vga_char (arg0) at location x (arg1) and y (arg2)
; returns the location of cursor after write
; arg0: char to write
; arg1: x location (x values greater than VGA_XSIZE will be ignored)
; arg2: y location (y values greater than VGA_YSIZE will be ignored)
; arg3: color (from vga.mac)
; ret0: x location
; ret1: y location
; ret2: color
vga_putchar: proc
  ; save coords
  push rbx
  push rcx
  push rdx
  ; ensure there are no out of bound mem writes
  cmp rbx, VGA_XSIZE
  jae .end
  cmp rcx, VGA_YSIZE
  jae .end

  ; preserve vga char and move xsize to accumulator
  mov rsi, rax
  mov rax, rcx

  ; find buffer index to write to
  mul VGA_XSIZE
  add rax, rbx

  ; rax now contains the index to write to
  ; write to location
  mov [VGA_BUF+rax*2], dl
  mov rdx, rsi
  mov [VGA_BUF+rax*2+1], dl

  ; pop em into the return locations
  pop rcx
  pop rbx
  pop rax

  ; if too much has been written on this line
  cmp rax, VGA_XSIZE
  jb .no_overflow
  inc rbx ; move to next line
  mov rax, 0 ; move x back
  .no_overflow:

  ; check if it is a newline character
  cmp rsi, '\n'
  jne .end

  ; this executes if newline
  inc rbx
  mov rax, 0
  .end:
endproc


; Displays string ptr arg0 with length arg1 to screen at cursor location
; arg0 pointer to string
; arg1 length of string
; arg2 cursor x
; arg3 cursor y
; arg4 cursor color
[GLOBAL vga_write]
vga_write: proc
  ; ensure there are no out of bound mem writes
  cmp rcx, VGA_XSIZE
  jae .end
  cmp rdx, VGA_YSIZE
  jae .end

  ; store string pointer safely in rdi
  mov rdi, rax

  ; calculate cursor beginning location in rax
  mov rax, VGA_XSIZE
  mul rdx
  add rax, rcx

  ; rax contains the cursor begin location

  ; check endpoint location of the write in r8
  mov r8, rax ; add startpoint
  add r8, rbx ; add length

  ; if its too large, we shrink it to the buf len
  cmp r8, VGA_BUF_LEN
  jbe .goodlength ; TODO is bug?
  ; set endpoint to the size
  mov r8, VGA_BUF_LEN
  .goodlength:

  ; subtract startpoint to get real length
  ; r8 now contains the actual length to write
  sub r8, rax

  ; copy the length to rcx
  mov rcx, r8
  ; copy the start location to rdx
  mov rdx, rax
  ; get pointer to destination buffer
  add rdx, rdx ; we are working with 2byte vga_chars
  add rdx, VGA_BUF
  ; first put color over everything
  .loop:
    ; move color byte
    mov [rdx], byte rsi
    inc rdx
    ; load char byte
    mov rax, [rdi]
    inc rdi
    ; write char byte
    mov [rdx], byte rax
    dec rcx
    ; ensure loop
    cmp rcx, 0
    jnz .loop
  .end:
endproc

; sets the color of the cursor
; arg0: cursor color (vga.mac)
; returns nothing
[GLOBAL vga_set_color]
vga_set_color: proc
  mov [color], al
endproc

[SECTION .data]
x: db 0
y: db 0
color: db vga_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)
