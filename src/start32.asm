[BITS 32]

; macros

%define DWORD_SIZE 4

%define argaddr(x) ebp + (x+2)*DWORD_SIZE
%define arg(x) [argaddr(x)]

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

; constants
[EXTERN MULTIBOOT_HEADER_MAGIC]
[EXTERN VGA_COLOR_BLACK]
[EXTERN VGA_COLOR_WHITE]
[EXTERN VGA_COLOR_RED]
[EXTERN VGA_XSIZE]
[EXTERN VGA_YSIZE]
[EXTERN VGA_BUFFER_LEN]
[EXTERN VGA_BUFFER_ADDR]
[EXTERN VGA_BUFFER_END_ADDR]

PAGE_SIZE equ 0x1000            ; 4096 bytes 
STACK_SIZE equ 0x4000           ; 16384 bytes (16 kb) for stack

; This is all executable code
[SECTION .text] 

; This function prints an error (arg0) to the screen and then halts forever
halt_with_error32: proc32
  ; First get vga color for space white foreground black background
  push VGA_COLOR_BLACK ; Background
  push VGA_COLOR_WHITE ; Foreground
  call vga_color32
  add esp, DWORD_SIZE*2 ; Pop stack

  ; push the resultant color
  push eax
  ; clear screen using this color
  call vga_clear_screen32

  ; Call print with first arg
  mov eax, arg(0)
  push eax
  call vga_print32
  add esp, DWORD_SIZE ; pop stack
  ; Now halt
  call halt32
endproc32


; This function hangs the cpu forever
halt32: proc32
  .hang:  hlt
    jmp .hang
endproc32


; This method will check if the cpu supports CPUID (1 if yes, 0 if no)
; No args
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
  mov eax, 0
  je .check_cpuid_support_end ; If they are equal tho, we can jump right to the end
  mov eax, 1 ; If it doesnt jump, we set it to 1
 .check_cpuid_support_end:
endproc32

; This will check if the cpu supports long mode (1 if yes, 0 if no) Make 
; sure to check for cpuid 
; No args
check_long_mode_support32: proc32
  mov eax, 0x80000000    ; Set the A-register to 0x80000000.
  cpuid                  ; CPU identification.
  cmp eax, 0x80000001    ; Compare the A-register with 0x80000001.
  jb .check_long_mode_support32_nolong ; It is less, there is no long mode.

  mov eax, 0x80000001    ; Set the A-register to 0x80000001.
  cpuid                  ; CPU identification.
  test edx, 1 << 29      ; Test if the LM-bit, (bit 29), is set in edx
  jz .check_long_mode_support32_nolong  ; They aren't, there is no long mode.

  mov eax, 1 ; Yes, it is supported
  jmp .check_long_mode_support32_end
  
  .check_long_mode_support32_nolong:
    mov eax, 0 ; No, it's not supported
    
  .check_long_mode_support32_end:
endproc32


; get vga color from foreground (arg0) and background (arg1)
; arg0: foreground color, as defined in vga.asm
; arg1: background color, as defined in vga.as,
vga_color32: proc32
  mov eax, arg(0) ; fg
  mov ecx, arg(1) ; bg
  shl ecx, 4 
  or  eax, ecx ; fg | bg << 4
endproc32

; create vga entry from character (arg0) and vga_color (arg1)
; arg0: 8 bit character
; arg1: vga_color created by vga_color32
vga_entry32: proc32
  mov eax, arg(0) ; character
  mov ecx, arg(1) ; vga_color
  shl ecx, 8 ; color << 8
  or eax, ecx ; character | color
endproc32

; This clears the screen with space characters with the given color arg0
; arg0: the color to clear the screen with
vga_clear_screen32: proc32

  mov eax, arg(0) ; arg0 is the color

  ; get vga entry using provided color
  ; push args for next proc
  push eax
  push ' ' ; Char to clear screen with
  call vga_entry32
  add esp, DWORD_SIZE*2 ; Pop stack 

  mov ecx, VGA_BUFFER_LEN ; ecx is counter

  push edi ; Preserve this register

  mov edi, VGA_BUFFER_ADDR ; Location to write to

  cld ; Copy forwards

  ; Fill in buffer with value
  rep stosw
  pop edi ; Restore edi 
endproc32

; Print vga chars to beginning, overwriting what is there, using pointer to null terminated string (arg0)
vga_print32: proc32
  mov edx, arg(0) ; Move the arg0 here
  mov ecx, 0 ; this register will serve as the counter
  .vga_print32_loop:
    mov al, [edx+ecx] ; Get a char from the string

    ; exit if null
    cmp al, 0
    je .vga_print32_end
    ; exit if we are going to exceed the limit 
    cmp ecx, VGA_BUFFER_LEN
    jge .vga_print32_end
    ; move char to buffer
    mov [VGA_BUFFER_ADDR+ecx*2], byte al 
    inc ecx
    jmp .vga_print32_loop
  .vga_print32_end:
endproc32


; Enables long mode compatibility mode using page table pointed to by arg0 
; It is still necessary to load a gdt after this to enter true 64 bit mode
; arg0 is a pointer to the p4 paging table
long_mode_compat_enable32: proc32
  ; move page table address to cr3
  mov eax, arg(0) ; arg0 is the p4 table
  mov cr3, eax

  ; enable PAE
  mov eax, cr4
  or eax, 1 << 5 ; physical address extension bit on cr4
  mov cr4, eax

  ; set the long mode bit
  mov ecx, 0xC0000080
  rdmsr
  or eax, 1 << 8 ; Enables long mode on model specific register
  wrmsr

  ; enable paging
  mov eax, cr0
  or eax, 1 << 31 ; set paging bit
  or eax, 1 << 16 ; set write protect bit
  mov cr0, eax

endproc32

; Start location, linker will start execution here
[GLOBAL start32]
start32:
  ; We haven't set up an IDT yet, so we'll disable interrupts for now
  cli

  ; To set up a stack, we set the esp register to point to the top of our
  ; stack (as it grows downwards on x86 systems).
  mov esp, stack_top

  cmp eax, MULTIBOOT_HEADER_MAGIC
  jne .has_multiboot ; If it was booted with multiboot
  ; this is if it does not match
  push errors.no_multiboot
  call halt_with_error32
  ; no return, so no need to clean up stack

  
  .has_multiboot:
  call check_cpuid_support32
  cmp eax, 0 
  jne .has_cpuid ; If its not zero
  ; However, if no cpuid, return error messge and halt
  push errors.no_cpuid
  call halt_with_error32

  ; If it does have cpuid we gotta check for long mode
 .has_cpuid:
  call check_long_mode_support32
  cmp eax, 0
  jne .has_long_mode
  ; However, if no long mode, return error messge and halt
  push errors.no_long_mode
  call halt_with_error32

  .has_long_mode:

  

  ; Kernel finished
  push errors.kernel_finished
  call halt_with_error32

 
[SECTION .data]

errors:
.no_multiboot: db 'Error: Kernel not booted with multiboot. Halting.',0
.no_cpuid: db 'Error: No CPUID support. Halting.',0
.no_long_mode: db 'Error: No support for long mode (64 bit). Halting.',0
.kernel_finished: db 'Error: Kernel exited. Halting.',0

; TODO this is another potential gdt that is a lot more opaque
;gdt64:
;    dq 0 ; zero entry
;.code: equ $ - gdt64
;    dq (1<<44) | (1<<47) | (1<<41) | (1<<43) | (1<<53) ; code segment
;.data: equ $ - gdt64
;    dq (1<<44) | (1<<47) | (1<<41) ; data segment
;.pointer:
;    dw $ - gdt64 - 1
;    dq gdt64

; From here
; https://wiki.osdev.org/Setting_Up_Long_Mode
gdt64:                           ; Global Descriptor Table (64-bit).
  .null: equ $ - gdt64           ; The null descriptor.
    dw 0xFFFF                    ; Limit (low).
    dw 0                         ; Base (low).
    db 0                         ; Base (middle)
    db 0                         ; Access.
    db 1                         ; Granularity.
    db 0                         ; Base (high).
  .code: equ $ - gdt64           ; The code descriptor.
    dw 0                         ; Limit (low).
    dw 0                         ; Base (low).
    db 0                         ; Base (middle)
    db 10011010b                 ; Access (exec/read).
    db 10101111b                 ; Granularity, 64 bits flag, limit19:16.
    db 0                         ; Base (high).
  .data: equ $ - gdt64           ; The data descriptor.
    dw 0                         ; Limit (low).
    dw 0                         ; Base (low).
    db 0                         ; Base (middle)
    db 10010010b                 ; Access (read/write).
    db 00000000b                 ; Granularity.
    db 0                         ; Base (high).
  .pointer:                      ; The gdt-pointer.
    dw $ - gdt64 - 1             ; Limit.
    dq gdt64                     ; Base.

; The bss section is uninitialized data
[SECTION .bss]

; Stack must be 16 byte aligned
align 16
stack_bottom:
resb STACK_SIZE ; 16 KiB
stack_top:

align PAGE_SIZE
p4_table:
    resb PAGE_SIZE
p3_table:
    resb PAGE_SIZE
p2_table:
    resb PAGE_SIZE
