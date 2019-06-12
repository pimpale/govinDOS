[BITS 32]
; Include externs
[EXTERN BOOT_HEADER_ADDR]
[EXTERN BOOT_LOAD_ADDR]
[EXTERN BOOT_LOAD_END_ADDR]
[EXTERN BOOT_BSS_END_ADDR]
[EXTERN start32]

; Declare constants for the multiboot header.
MULTIBOOT_PAGE_ALIGN		equ 1<<0 ; All boot modules loaded along with the operating system must be aligned on page (4KB) boundaries.
MULTIBOOT_MEMORY_INFO		equ 1<<1 ; Let the bootloader fill in the mem_ fields.
MULTIBOOT_AOUT_KLUDGE		equ 1<<16 ; Let the bootloader use something other than elf 
MULTIBOOT_HEADER_MAGIC		equ 0x1BADB002
MULTIBOOT_HEADER_FLAGS		equ MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_AOUT_KLUDGE		
MULTIBOOT_HEADER_CHECKSUM	equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)
 
; Declare a multiboot header that marks the program as a kernel. These are magic
; values that are documented in the multiboot standard. The bootloader will
; search for this signature in the first 8 KiB of the kernel file, aligned at a
; 32-bit boundary. The signature is in its own section so the header can be
; forced to be within the first 8 KiB of the kernel file.
[SECTION .header]
align 4
  dd MULTIBOOT_HEADER_MAGIC
  dd MULTIBOOT_HEADER_FLAGS
  dd MULTIBOOT_HEADER_CHECKSUM
  ; AOUT kludge
  dd BOOT_HEADER_ADDR ; point to multiboot header
  dd BOOT_LOAD_ADDR ;state of kernel .text (code) section
  dd BOOT_LOAD_END_ADDR ; start of bss section
  dd BOOT_BSS_END_ADDR ; end of bss section
  dd start32 ; kernel entry point (initial EIP)
