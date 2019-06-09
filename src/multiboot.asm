bits 32
; Include externs
extern header_addr
extern load_addr
extern load_end_addr
extern bss_end_addr
extern start

; Declare constants for the multiboot header.
MULTIBOOT_PAGE_ALIGN		equ 1<<0 ; All boot modules loaded along with the operating system must be aligned on page (4KB) boundaries.
MULTIBOOT_MEMORY_INFO		equ 1<<1 ; Let the bootloader fill in the mem_ fields.
MULTIBOOT_HEADER_MAGIC		equ 0x1BADB002
MULTIBOOT_HEADER_FLAGS		equ MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO 
MULTIBOOT_HEADER_CHECKSUM	equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)
 
; Declare a multiboot header that marks the program as a kernel. These are magic
; values that are documented in the multiboot standard. The bootloader will
; search for this signature in the first 8 KiB of the kernel file, aligned at a
; 32-bit boundary. The signature is in its own section so the header can be
; forced to be within the first 8 KiB of the kernel file.
section .multiboot

align 4
	dd MULTIBOOT_HEADER_MAGIC
	dd MULTIBOOT_HEADER_FLAGS
	dd MULTIBOOT_HEADER_CHECKSUM

