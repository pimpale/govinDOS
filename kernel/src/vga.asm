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
color: dw VGA_COLOR_WHITE_FG | VGA_COLOR_BLACK_BG
