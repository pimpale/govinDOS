# Application Binary Interface

GovinDOS is an only 64 bit operating system. While there are some 32 bit functions in the code, they are confined only to the start32 and 32 assembly files in the kernel. These files use the cdecl calling convention. However, excepting these few functions in the early stage kernel, everything else follows the GovinDOS Application Binary Interface. For the most part, the ABI x86_64 GovinDOS is identical to the System V ABI with a few exceptions: 

* Scratch registers are rax, rbx, rcx, rdx, rsi, rdi, r8, r9
* Preserved registers are rbp, rsp, r10, r11, r12, r13, r13, r14, r15
* The first 8 Integer arguments (char, short, int, long) are placed in the scratch registers in the order rax, rbx, rcx, rdx, rsi, rdi, r8, r9. The rest are placed onto the stack (Right to left)
* Return values are passed in the scratch registers, in the above order. This allows returning multiple values from a function. However, this is only up to 8. It is not possible to return more than 8 values from a function.
* TODO how do we return structs? 

In addition, for variadic functions, the first argument shall be the number of values provided (not including this first argument)
Similarily, for variadic returns, the first return value shall be the number of values returned (not including this first return). 

### Examples

#### Multiple Return Values
In this example a function that returns multiple values (malloc) is defined. Malloc returns either a memory address and no error or a memory address with an error value. In doing so, we are able to provide more information to a caller, with negligible cost on our part
The foo function calls malloc and handles a potential error.

```nasm
; /* the following function mallocs and provides an error code */ 
; /* Nothing is variadic, everything is predetermined */ 
; (Void*, U64) malloc(U64 size) {
;   /* 
;    * Do malloc stuff here
;    */
;   if(has_error) { 
;     return(NULL, error_code);
;   } else {
;     return(mem_addr, NO_ERROR);
;   }
; }

malloc:
  ; Preserve the old base pointer and then set it
  ; This also aligns the stack since RIP has been pushed. 
  ; Pushing rbp allows it to be once more 16 byte aligned
  push rbp
  mov rbp, rsp

  ; Do malloc stuff here. We assume, by the end that rax 
  ; has an error code, and that rdx has the memory address 

  ; check if there's been an error
  cmp rax, NO_ERROR
  je .noerror
  ; this is what happens if there's an error
  ; (meaning that rax has a value not 0)
  
  mov rbx, rax ; The second return value is the error code 
  mov rax, PTR_NULL ; The first return value is null

  jmp .end
  .noerror
  ; This shall run if there wasnt an error
  mov rbx, NO_ERROR
  mov rax, rdx ; we put the memory address as the first return arg
  
  .end
  ; We pop rbp back where it belongs and return
  pop rbp
  ret


; /* the following function calls malloc */
; Void foo() {
;   U64* array, err = malloc(500);
;   if(err != NO_ERROR) {
;     puts("memory alloc failed");
;     exit(1);
;   } else {
;     /* Do something with array */
;     puts("success");
;   }
; }

foo:   
  ; Once again, we must set up the stack frame
  push rbp
  mov rbp, rsp

  ; Set the first argument to 500
  mov rax, 500 
  call malloc

  ; check if there's been an error
  cmp rbx, NO_ERROR
  je .noerror

  ; the following code will run only on error

  ; print the fail message
  mov rax, MEM_ALLOC_FAIL_MSG
  call puts
  ; Exit with 1 status 
  mov rax, 1 
  call exit
  jmp .end

  .noerror:

  ; Do something with array (rax)

  mov rax, MEM_ALLOC_SUCCEED_MSG
  call puts

  .end:
  ; end stack frame
  pop rbp
  ret

section .data
MEM_ALLOC_SUCCEED_MSG: db "success",0
MEM_ALLOC_FAIL_MSG: db "memory alloc failed",0


```

#### Variadic Arguments
The following example shows an example of a variadic function. The function printf accepts a variadic number of arguments It then processes these arguments, returning an error if the arguments are incorrectly formatted. The foo function calls printf.

```nasm
; /* printf: Prints formatted arguments */
; Error printf(U64 argc, Void** argv){
;   if(argc == 0) {
;     return(INVALID_ARGS);
;   }
;   /*
;    * Do printf stuff 
;    */
; }

printf:
  ; Create stack frame
  push rbp
  mov rbp, rsp

  ; check if argc is 0
  cmp rax, 0
  jne .validargs
  mov rax, INVALID_ARGS
  jmp .end

  .validargs:

  ; Do printf stuff

  mov rax, NO_ERROR

  .end:
  pop rbp
  ret

; Void foo() {
;   printf("hello world, %d", 5);
; }
foo: 
  ; set up stack frame
  push rbp
  mov rbp, rsp

  mov rax, 2
  mov rbx, HELLO_WORLD_MSG
  mov rcx, 5
  call printf
  
  pop rbp
  ret

section .data
HELLO_WORLD_MSG: db "hello world, %d",0

```


