[GLOBAL MULTIBOOT_PAGE_ALIGN]
[GLOBAL MULTIBOOT_MEMORY_INFO]
[GLOBAL MULTIBOOT_AOUT_KLUDGE]
[GLOBAL MULTIBOOT_HEADER_MAGIC]
[GLOBAL MULTIBOOT_BOOT_REGISTER_MAGIC]

; Declare constants for the multiboot header.
MULTIBOOT_PAGE_ALIGN          equ 1<<0  ; everything is aligned 4096
MULTIBOOT_MEMORY_INFO         equ 1<<1  ; grub gets sys mem info
MULTIBOOT_AOUT_KLUDGE         equ 1<<16 ; bootloader load non-elf 
MULTIBOOT_HEADER_MAGIC        equ 0x1BADB002 ; magic value to put in header
MULTIBOOT_BOOT_REGISTER_MAGIC equ 0x2BADB002 ; magic value to be stored in ebx
