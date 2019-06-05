bits 32


extern halt_with_error32
extern vga_clear_screen32
extern vga_print32
extern check_cpuid_support32
extern check_long_mode_support32



; Stack must be 16 byte aligned
section .bss
align 16
stack_bottom:
resb 16384 ; 16 KiB
stack_top:
 
; Start location, linker will start execution here
section .text
global start
start:
  ; We haven't set up an IDT yet, so we'll disable interrupts for now
	cli

	; To set up a stack, we set the esp register to point to the top of our
	; stack (as it grows downwards on x86 systems).
	mov esp, stack_top

  ; First clear screen
	call vga_clear_screen32

  
  call check_cpuid_support32
  cmp eax, 0 
  jne .has_cpuid ; If its not zero
  ; However, if no cpuid, return error messge and halt
  push no_cpuid_error_message
  call halt_with_error32

  ; If it does have cpuid we gotta check for long mode
 .has_cpuid:
  call check_long_mode_support32
  cmp eax, 0
  jne .has_long_mode
  ; However, if no long mode, return error messge and halt
  push no_long_mode_error_message
  call halt_with_error32

  .has_long_mode
  ; Success
  push success_error_message
  call halt_with_error32

 


section .rodata
no_cpuid_error_message: db 'Error: No CPUID support. Halting.',0
no_long_mode_error_message: db 'Error: No support for long mode (64 bit). Halting.',0
success_error_message: db 'Error: Success. Halting.',0
