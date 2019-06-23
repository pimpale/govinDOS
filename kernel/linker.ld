OUTPUT_FORMAT("binary")
ENTRY(early_init)
SECTIONS
{
  /* Where the kernel physically is in memory */
  KERNEL_PHYS_ADDR = 0x10000;

  /* We start 1M away */
  . += KERNEL_PHYS_ADDR;

  .text :
  {
    BOOT_LOAD_ADDR = .;
    BOOT_HEADER_ADDR = .;
    *(.header);
    *(.text*);
    . = ALIGN(4K);
  }

  .data : 
  {
    *(.data*);
    BOOT_LOAD_END_ADDR = .;
    . = ALIGN(4K);
  }

  .bss : 
  {
    BOOT_BSS_ADDR = .;
    *(.bss*);
    *(COMMON);
    BOOT_BSS_END_ADDR = .;
    . = ALIGN(4K);
  }
}
