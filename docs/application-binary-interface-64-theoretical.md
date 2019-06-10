# Application Binary Interface

The Application Binary Interface for GovinDOS varies significantly from others. Many return values are supported, and all return values and parameters are placed on the stack. 
Following are the most important rules for the calling convention

The stack shall be aligned to 16 bytes.
Parameters shall be pushed onto the stack from right to left.
Parameters shall be passed 8 byte aligned. 
RAX, R, RCX, and RDX shall be scratch, the rest shall be preserved
The callee must preserve all other registers
The caller must push arguments onto the stack
The callee must clean the arguments from the stack. 
The callee must push return values onto the stack, right to left, 8 byte aligned

## Example
```nasm
[BITS 64]

; The function foo accepts 3 64bit parameters 
; It returns two tSums and adds all of them
; (U64, U64) foo(U64 a, U64 b, U64 c) {
;   return (a+b+c, a*b*c);
; }
foo: 
  push rbp     ; Preserve old base pointer
  mov rbp, rsp ; Make new frame
  mov rax, [rbp + (0+2)*8] ; move a to rax
  mov rax, [rbp + (1+2)*8] ; move b to rax
  mov rcx, [rbp + (2+2)*8] ; move c to rax
  mov rdx, 0 ; zero rdx
  add rdx, rax
  add rdx, rbx
  add rdx, rcx
  push rdx
  mov rdx, 0 ; zero rdx in preparation for mul instructions
  mul rbx
  mul rcx
  ; We ignore possiblity of overflow
  push rax
  ; TODO??? how do i push args onto the stack after ret?


main:
  push qword 5 ; c
  push qword 3 ; b
  push qword 2 ; a
  call foo
  pop eax ; return arg 2 
  pop ecx ; return arg 1 


  










