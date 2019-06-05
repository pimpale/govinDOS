DWORD_SIZE equ 4

%define arg(x) [ebp + (x+2)*DWORD_SIZE]

; Preserves base pointer and sets it
%macro proc32 0
  push ebp 
  mov ebp, esp 
%endmacro

; Ends procedure and returns
%macro endproc32 0
  pop ebp ; restore base pointer
  ret
%endmacro

extern VGA_COLOR_BLACK
extern VGA_COLOR_WHITE
extern VGA_COLOR_RED
extern VGA_XSIZE
extern VGA_YSIZE
extern VGA_BUFFER_ADDR


bits 32
section .text 

; This method will check if the cpu supports CPUID (1 if yes, 0 if no)
; No Args
global check_cpuid_support32
check_cpuid_support32: proc32
  ; Check if CPUID is supported by attempting to flip the ID bit (bit 21) in
  ; the FLAGS register. If we can flip it, CPUID is available.

  ; Copy FLAGS in to EAX via stack
  pushfd
  pop eax
  ; Copy to ECX as well for comparing later on
  mov ecx, eax
  ; Flip the ID bit
  xor eax, 1 << 21
  ; Copy EAX to FLAGS via the stack
  push eax
  popfd
  ; Copy FLAGS back to EAX (with the flipped bit if CPUID is supported)
  pushfd
  pop eax
  ; Restore FLAGS from the old version stored in ECX (i.e. flipping the ID bit
  ; back if it was ever flipped).
  push ecx
  popfd
  ; Compare EAX and ECX. If they are equal then that means the bit wasn't
  ; flipped, and CPUID isn't supported.
  cmp eax, ecx
  je .check_cpuid_support_no

  .check_cpuid_support_yes: ; returns 1
    mov eax, 1

  .check_cpuid_support_no:  ; returns 0
    mov eax, 0
endproc32

; This will check if the cpu supports long mode (1 if yes, 0 if no) Make sure to check for cpuid 
; No Args
global check_long_mode_support32
check_long_mode_support32: proc32
  mov eax, 0x80000000    ; Set the A-register to 0x80000000.
  cpuid                  ; CPU identification.
  cmp eax, 0x80000001    ; Compare the A-register with 0x80000001.
  jb .check_long_mode_support32_nolong ; It is less, there is no long mode.

  mov eax, 0x80000001    ; Set the A-register to 0x80000001.
  cpuid                  ; CPU identification.
  test edx, 1 << 29      ; Test if the LM-bit, which is bit 29, is set in the D-register.
  jz .check_long_mode_support32_nolong  ; They aren't, there is no long mode.

  mov eax, 1 ; Yes, it is supported
  jmp .check_long_mode_support32_end
  
  .check_long_mode_support32_nolong:
    mov eax, 0 ; No, it's not supported
    
  .check_long_mode_support32_end:
endproc32


; get vga color from foreground (arg0) and background (arg1)
vga_color32: proc32
  mov eax, arg(0) ; fg
  mov ecx, arg(1) ; bg
  shl ecx, 4 
  or  eax, ecx ; fg | bg << 4
endproc32

; create vga entry from character (arg0) and vga_color (arg1)
vga_entry32: proc32
  mov eax, arg(0) ; character
  mov ecx, arg(1) ; vga_color
  shl ecx, 8 ; color << 8
  or eax, ecx ; character | color
endproc32

; This clears the screen to black
global vga_clear_screen32
vga_clear_screen32: proc32

  ; First get vga color for space white foreground black background
  push VGA_COLOR_BLACK ; Background
  push VGA_COLOR_WHITE ; Foreground
  call vga_color32
  add esp, DWORD_SIZE*2 ; Pop stack
  
  ; push args for next proc
  push eax
  push ' ' ; Char to clear screen with

  ; Then get vga entry
  call vga_entry32
  add esp, DWORD_SIZE*2 ; Pop stack 

  mov ecx, eax ; Store value safely

  ; Finally multiply eax and edx to get size
  mov eax, VGA_XSIZE 
  mov edx, VGA_YSIZE
  mul edx 

  ; Prep for fill

  ; eax is the size of screen
  ; ecx is value to fill is the vga_entry
  xchg eax, ecx ; we need to swap to be correct

  dec ecx 

  push edi ; Preserve this register

  mov edi, VGA_BUFFER_ADDR ; Location to write to

  cld ; Copy forwards

  ; Fill in buffer with value
  rep stosw
  pop edi ; Restore 
endproc32

; Print vga chars to beginning, overwriting what is there, using pointer to null terminated string (arg0)
global vga_print_error32
vga_print_error32: proc32

  push ebx ; We will be using ebx, save that

  ; First get vga color for space white foreground black background
  push VGA_COLOR_BLACK
  push VGA_COLOR_WHITE
  call vga_color32
  add esp, DWORD_SIZE*2 ; Pop args
  ; eax is the color
  
  mov ebx, arg(0) ; ebx is current pointer to string
  mov ecx, 0 ; ecx is the counter

  .vga_print_error32_loop: 

    mov dl, [ebx + ecx] ; load byte of string counter away

    ; If null byte, exit
    cmp edx, 0
    je .vga_print_error32_end

    ; Save sratch regs
    push eax ; save color
    push ebx ; save pointer
    push ecx ; save counter

    ; Get vga entry
    push eax ; push color
    push edx ; push char
    call vga_entry32 
    add esp, DWORD_SIZE*2 ; Pop stack 

    pop ecx ; Restore  counter

    mov [VGA_BUFFER_ADDR+ecx], ax ; Move vga entry to buffer
    pop ebx ; Restore string pointer
    pop eax ; restore color
    ; increment counter
    inc ecx
    jmp .vga_print_error32_loop


  .vga_print_error32_end:
    pop ebx
endproc32
