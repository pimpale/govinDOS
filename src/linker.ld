OUTPUT_FORMAT("binary")
ENTRY(start32)
SECTIONS
{
  /* Where the kernel will be mapped to */
  KERNEL_VIRT_ADDR = 0xFFFFFFFFC0000000; 
  /* Where the kernel physically is in memory */
  KERNEL_PHYS_ADDR = 0x10000;

  /* We start 1M away */
  . += KERNEL_PHYS_ADDR;

  /* Shove all the boot + paging stuff in the beginning*/
  /* All 32 bit code uses cdecl and has no paging nor heap whatsoever. */
  .32text :
  {
    BOOT_HEADER_ADDR = .;
    *32.o (.header)
    BOOT_LOAD_ADDR = .;
    *32.o (.text)
  }

  .32data :
  {
    *32.o (.data)
    BOOT_LOAD_END_ADDR = .;
  }

  .32bss :
  {
    BOOT_BSS_ADDR = .;
    *32.o (.bss)
    *32.o (COMMON)
    BOOT_BSS_END_ADDR = .;
  }



  /* The rest of the stuff will have to be mapped before we can access it */
  . += KERNEL_VIRT_ADDR;

  /* the at means that it is physically placed without the offset */
  /* However, all jumps and location will assume it is over here */
  /* This is fine, because paging will have been enabled */


  .text : AT(ADDR(.text) - KERNEL_VIRT_ADDR)
  {
    *(.text*);
    . = ALIGN(4K);
  }

  .data : AT(ADDR(.data) - KERNEL_VIRT_ADDR)
  {
    *(.data*);
    . = ALIGN(4K);
  }

  .bss : AT(ADDR(.bss) - KERNEL_VIRT_ADDR)
  {
    *(.bss*);
    *(COMMON);
    . = ALIGN(4K);
  }
}
