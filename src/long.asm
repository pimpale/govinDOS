%include "ccall.mac"


bits 32
section .text 

; This method will check if the cpu supports CPUID (1 if yes, 0 if no)
global check_cpuid_support_32
check_cpuid_support_32: proc
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
endproc

; This will check if the cpu supports long mode (1 if yes, 0 if no) Make sure to check for cpuid 
global check_long_mode_support_32
check_long_mode_support_32: proc
  mov eax, 0x80000000    ; Set the A-register to 0x80000000.
  cpuid                  ; CPU identification.
  cmp eax, 0x80000001    ; Compare the A-register with 0x80000001.
  jb .check_long_mode_support_32_nolong ; It is less, there is no long mode.

  mov eax, 0x80000001    ; Set the A-register to 0x80000001.
  cpuid                  ; CPU identification.
  test edx, 1 << 29      ; Test if the LM-bit, which is bit 29, is set in the D-register.
  jz .check_long_mode_support_32_nolong  ; They aren't, there is no long mode.

  mov eax, 1 ; Yes, it is supported
  jmp .check_long_mode_support_32_end
  
  .check_long_mode_support_32_nolong:
    mov eax, 0 ; No, it's not supported
    
  .check_long_mode_support_32_end:
endproc



