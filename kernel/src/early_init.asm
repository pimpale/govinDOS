[BITS 32]

; constants
%include "call.mac"
%include "vga.mac"
%include "cr.mac"
%include "cpuid.mac"
%include "multiboot.mac"
%include "common.mac"

%define PAGE_SIZE  0x1000           ; 4096 bytes
%define STACK_SIZE 0x4000           ; 16384 bytes (16 kb) for stack

[EXTERN init]
[EXTERN main]

[EXTERN gdt]
[EXTERN gdt.code]
[EXTERN gdt.data]
[EXTERN gdt.pointer]

; This is all executable code
[SECTION .text]

; This function prints an error (arg0) to the screen and then halts forever
early_halt_with_error: proc32
  ; First get vga color for space white foreground black background
  push VGA_COLOR_BLACK ; Background
  push VGA_COLOR_WHITE ; Foreground
  call early_vga_color
  add esp, DWORD_SIZE*2 ; Pop stack

  ; push the resultant color
  push eax
  ; clear screen using this color
  call early_vga_clear_screen

  ; Call print with first arg
  mov eax, arg32(0)
  push eax
  call early_vga_print
  add esp, DWORD_SIZE ; pop stack
  ; Now halt
  call early_halt
endproc32


; This function hangs the cpu forever
early_halt: proc32
  .hang:  hlt
    jmp .hang
endproc32

; This will check if the cpu supports long mode (1 if yes, 0 if no) Make
; sure to check for cpuid
; No args
early_check_long_mode_support: proc32
  mov eax, CPUID_GET_EXT_FUNC          ; Set the A-register to 0x80000000.
  cpuid                                ; CPU identification.
  cmp eax, CPUID_GET_EXT_PROC_INFO     ; Compare the A-register with 0x80000001.
  jb .nolong ; It is less, there is no long mode.

  mov eax, CPUID_GET_EXT_PROC_INFO     ; Set the A-register to 0x80000001.
  cpuid                                ; CPU identification.
  test edx, CPUID_EXT_FEAT_EDX_LM      ; Test if the LM-bit, (bit 29), is set in edx
  jz .nolong ; They aren't, there is no long mode.

  mov eax, 1 ; Yes, it is supported
  jmp .end

  .nolong:
    mov eax, 0 ; No, it's not supported

  .end:
endproc32


; get vga color from foreground (arg0) and background (arg1)
; arg0: foreground color, as defined in vga.asm
; arg1: background color, as defined in vga.asm
early_vga_color: proc32
  mov eax, arg32(0) ; fg
  mov ecx, arg32(1) ; bg
  shl ecx, 4
  or  eax, ecx ; fg | bg << 4
endproc32

; create vga entry from character (arg0) and vga_color (arg1)
; arg0: 8 bit character
; arg1: vga_color created by vga_color32
early_vga_entry: proc32
  mov eax, arg32(0) ; character
  mov ecx, arg32(1) ; vga_color
  shl ecx, 8 ; color << 8
  or eax, ecx ; character | color
endproc32

; This clears the screen with space characters with the given color arg0
; arg0: the color to clear the screen with
early_vga_clear_screen: proc32

  mov eax, arg32(0) ; arg0 is the color

  ; get vga entry using provided color
  ; push args for next proc
  push eax
  push ' ' ; Char to clear screen with
  call early_vga_entry
  add esp, DWORD_SIZE*2 ; Pop stack

  mov ecx, VGA_BUF_LEN ; ecx is counter

  push edi ; Preserve this register

  mov edi, VGA_BUF ; Location to write to

  cld ; Copy forwards

  ; Fill in buffer with value
  rep stosw
  pop edi ; Restore edi
endproc32

; Print vga chars to beginning, overwriting what is there, using pointer to null terminated string (arg0)
early_vga_print: proc32
  mov edx, arg32(0) ; Move the arg0 here
  mov ecx, 0 ; this register will serve as the counter
  .loop:
    mov al, [edx+ecx] ; Get a char from the string

    ; exit if null
    cmp al, 0
    je .end
    ; exit if we are going to exceed the limit
    cmp ecx, VGA_BUF_LEN
    jge .end
    ; move char to buffer
    mov [VGA_BUF+ecx*2], byte al
    inc ecx
    jmp .loop
  .end:
endproc32


; Enables long mode compatibility mode using page table pointed to by arg0
; It is still necessary to load a gdt after this to enter true 64 bit mode
; arg0 is a pointer to the p4 paging table
early_long_mode_compat_enable: proc32
  ; move page table address to cr3
  mov eax, arg32(0) ; arg0 is the p4 table
  mov cr3, eax

  ; enable PAE
  mov eax, cr4
  or eax, CR4_PAE ; physical address extension bit on cr4
  mov cr4, eax

  ; TODO pls export this to a file so we dont have magic numbers
  ; set the long mode bit
  mov ecx, 0xC0000080
  rdmsr
  or eax, 1 << 8 ; Enables long mode on model specific register (msr)
  wrmsr

  ; enable paging
  mov eax, cr0
  or eax, CR0_PG ; set paging bit
  or eax, CR0_WP ; set write protect bit
  mov cr0, eax
endproc32

; Initializes a page table identity mapping the first 2 MiB with huge pages
;
; This table is necessary to enter long mode, and should be replaced
; by the 64bit kernel. Since we are mapping only a tiny amount of
; space for the initial kernel, all we really need is a few MiB.
; Hence, we only accept 1 of each level.
;
; arg0 pointer to uninitialized memory for page table level 2
; arg1 pointer to uninitialized memory for page table level 3
; arg2 pointer to uninitialized memory for page table level 4
; returns nothing
early_init_early_table: proc32
  ; TODO less hacks, need to remove magic numbers.

  ; Point the first entry of the level 4 page table to the first entry in the
  ; p3 table
  mov eax, arg32(1) ;p3_table
  or eax, 11b ;
  mov ecx, arg32(2) ; p4_table
  mov dword [ecx + 0], eax

  ; Point the first entry of the level 3 page table to the first entry in the
  ; p2 table
  mov eax, arg32(0) ;p2_table
  or eax, 11b
  mov ecx, arg32(1) ; p3_table
  mov dword [ecx + 0], eax

  ; point each page table level two entry to a page
  mov ecx, 0         ; counter variable
  .map_p2_table:
    mov eax, 0x200000  ; 2MiB
    mul ecx
    or eax, 10000011b
    mov [p2_table + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_table

endproc32

; Start location, linker will start execution here
[GLOBAL early_init]
early_init:
  ; We haven't set up an IDT yet, so we'll disable interrupts for now
  cli

  ; To set up a stack, we set the esp register to point to the top of our
  ; stack (as it grows downwards on x86 systems).
  mov esp, stack_top

  cmp eax, MULTIBOOT_BOOT_REGISTER_MAGIC ; multiboot1 magic value
  je .has_multiboot ; If it was booted with multiboot
  ; this is if it does not match
  push errors.no_multiboot
  call early_halt_with_error
  ; clean stack (wont be run, but is good practice regardless)
  add esp, DWORD_SIZE

  .has_multiboot:

  ; check for long mode
  call early_check_long_mode_support
  cmp eax, 0
  jne .has_long_mode
  ; However, if no long mode, return error messge and halt
  push errors.no_long_mode
  call early_halt_with_error
  add esp, DWORD_SIZE

  .has_long_mode:
  ; then proceed to set up identity paging of the tables

  ; Initialize tables
  push p4_table
  push p3_table
  push p2_table
  call early_init_early_table
  add esp, DWORD_SIZE*3

  ; Now initialize compat mode
  push p4_table
  call early_long_mode_compat_enable
  add esp, DWORD_SIZE


  lgdt [gdt.pointer]
  ; update selectors
  mov ax, gdt.data
  mov ss, ax
  mov ds, ax
  mov es, ax

  jmp gdt.code:init


[SECTION .data]

errors:
  .no_multiboot:
    db 'Error: Kernel not booted with multiboot. Halting.',0
  .no_long_mode:
    db 'Error: No support for long mode (64 bit). Halting.',0

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
