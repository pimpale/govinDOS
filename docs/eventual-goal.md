[/usr/anon]$ 
[/usr/anon]$ lc
readme.txt 
etc/
example.gef*
example.asm
[/usr/anon]$ cat example.asm
[bits 64]
push rax ; Pointer to char* array
push rcx ; Pointer to 

// TODO figure out calling convention first
// This is going to be a very simple hello world

[/usr/anon]$ assemble example.asm example.obj
[/usr/anon]$ link -lstdlib example.obj example
[/usr/anon]$ ./example
Hello World
[/usr/anon]$ 
