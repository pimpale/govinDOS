[BITS 32]
; Include and externs

%include "multiboot.mac"

[EXTERN BOOT_HEADER]
[EXTERN BOOT_LOAD]
[EXTERN BOOT_LOAD_END]
[EXTERN BOOT_BSS_END]
[EXTERN early_init]

MULTIBOOT_HEADER_FLAGS    equ MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_AOUT_KLUDGE
MULTIBOOT_HEADER_CHECKSUM equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

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
  dd BOOT_HEADER ; point to multiboot header
  dd BOOT_LOAD ;state of kernel .text (code) section
  dd BOOT_LOAD_END ; start of bss section
  dd BOOT_BSS_END ; end of bss section
  dd early_init ; kernel entry point (initial EIP)
