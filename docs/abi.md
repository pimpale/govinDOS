# Application Binary Interface

GovinDOS is an only 64 bit operating system. While there are some 32 bit functions in the code, they are confined only to the start32 and 32 assembly files in the kernel. These files use the cdecl calling convention. However, excepting these few functions in the early stage kernel, everything else follows the GovinDOS Application Binary Interface. For the most part, the ABI x86_64 GovinDOS is identical to the System V ABI with a few exceptions: 

* Scratch registers are rax, rbx, rcx, rdx, rsi, rdi, r8, r9
* Preserved registers are rbp, rsp, r10, r11, r12, r13, r13, r14, r15
* The first 8 Integer arguments (char, short, int, long) are placed in the scratch registers in the order rax, rbx, rcx, rdx, rsi, rdi, r8, r9. The rest are placed onto the stack (Right to left)
* Return values are passed in the scratch registers, in the above order. This allows returning multiple values from a function. However, this is only up to 8. Beyond that, the caller must allocate space for the return arguments in the stack, above the arguments.  It is not possible to return a variable amount of values from a function.
* Structs shall be broken down into their respective 

In addition, for variadic functions, the first argument shall be the number of values provided (not including this first argument)

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
  ; Preserve the old frame pointer and then set it
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
; U64 printf(U64 argc, Void** argv){
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

  ; the following runs if error
  mov rax, INVALID_ARGS
  jmp .end

  .validargs:
  ; everything under here will run if no error

  ; Do printf stuff

  mov rax, NO_ERROR

  .end:
  ; destroy stack frame
  pop rbp
  ret

; Void foo() {
;   printf("hello world, %d", 5);
; }
foo: 
  ; set up stack frame
  push rbp
  mov rbp, rsp

  ; Move the arguments into place

  mov rax, 2 ; the total number of args to follow (always required)
  mov rbx, HELLO_WORLD_MSG ; The format string
  mov rcx, 5 ; the value to sub in
  call printf
  
  ; Destroy stack frame
  pop rbp
  ret

section .data
HELLO_WORLD_MSG: db "hello world, %d",0

```

#### Many arguments and return values
The following example shows an example of a function utilizing structs, demonstrating the correct way to handle them The struct overflows the registers and must be partially stored in the stack. Each field of the struct is stored in a register. Function increment accepts a large struct, performs an operation on a struct, and then proceeds to return it. increment is called by the function foo.


```nasm

; /* A struct containing 10 values */
; typedef struct {
;   U64 var1;
;   U64 var2;
;   U64 var3;
;   U64 var4;
;   U64 var5;
;   U64 var6;
;   U64 var7;
;   U64 var8;
;   U64 var9;
;   U64 var10;
; } Dectet;
; 
; /* Foolish mechanism to increment every field on a struct */
; Dectet increment(Dectet src) {
;   src.var1++;
;   src.var2++;
;   src.var3++;
;   src.var4++;
;   src.var5++;
;   src.var6++;
;   src.var7++;
;   src.var8++;
;   src.var9++;
;   src.var10++;
;   return src;
; }

increment:
  ; We preserve the old frame pointer and create a new frame
  push rbp
  mov rbp, rsp

  ; increment everything held on the registers
  inc rax
  inc rbx
  inc rcx
  inc rdx
  inc rsi
  inc rdi
  inc r8
  inc r9
  
  ; We'll need to save the value of a register or two so we can use 
  ; it to do other stuff
  push r10

  ; save src.var9 in r10
  ; In the govinDOS ABI the stack arguments will start at the base pointer 
  ; plus some space for RIP and RBP (that is why we have the +2) Since 
  ; each of these values is a qword, that gives us 16 bytes of offset
  mov r10, [rbp + (0+2)*8]
  inc r10

  ; Now we must put this into the space reserved for the return arguments 
  ; by the caller. The caller knows that 2 qwords of stack space is reserved 
  ; for the remainder of this massive struct. But above this is space for the
  ; two qwords of the struct that don't fit on the return stack.
  ; We will copy the value to that location. This location is 4 qwords away
  ; 2 for rip and rbp, and 2 for the arguments

  mov [rbp + (0+4)*8], r10 ; var9

  ; repeat the process for the second arg

  mov r10, [rbp + (1+2)*8] ; var10
  inc r10
  mov [rbp + (1+4)*8], r10

  ; now we can restore r10
  pop r10

  ; Destroy stack frame
  pop rbp
  ret


; /* 
;  * Foo initializes the Dectet to 0, and then makes a call to increment.
;  * Why memset was not used, we may never know.
;  */
; Void foo() {
;   Dectet d = { 0 };
;   d = increment(d);
;   d = increment(d);
;   /* alternatively
;    * d.increment().increment();
;    * in this C variant
;    */
; }
foo:
  ; Create stack frame
  push rbp
  mov rbp, rsp

  ; free a few registers to work with
  push r10
  push r11

  ; we need to create space for the function's return
  ; We give enough space for 2 qwords, 16 bytes. This is var9 and var10
  
  sub rsp, 2*8

  ; zero rax
  mov rax, 0 ; var1

  ; now zero all other registers
  mov rbx, rax ; var2
  mov rcx, rax ; var3
  mov rdx, rax ; var4
  mov rsi, rax ; var5
  mov rdi, rax ; var6
  mov r8, rax  ; var7
  mov r9, rax  ; var8

  ; Since there are two more args, we need to push them onto the stack
  push rax ; var10
  push rax ; var9

  ; Now we can finally call increment
  call increment
  ; drop old args
  add rsp, 2*8
  
  ; we can reuse the space we used for the return args of this function 
  ; for the next function call's return space. however, we do need to push 
  ; the the args, which are the returns of the just called func

  ; we've already saved r10 and r11

  pop r10 ; var9
  pop r11 ; var10

  ; save space for returns
  sub rsp, 2*8

  ; push args
  push r11 ; var10
  push r10 ; var0

  ; call increment again
  call increment

  ; we can discard this return because we wont be using it for anything
  ; drop args and return values
  add rsp, 4*8

  ; restore r10 and r11
  pop r11
  pop r10

  ; destroy stack frame and exit procedure
  pop rbp
  ret
```
