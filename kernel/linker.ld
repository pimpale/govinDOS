OUTPUT_FORMAT("binary")
ENTRY(early_init)
SECTIONS
{
  /* Where the kernel physically is in memory */
  KERNEL_PHYS_PTR = 0x10000;

  /* We start 1M away */
  . += KERNEL_PHYS_PTR;

  .text :
  {
    BOOT_LOAD_PTR = .;
    BOOT_HEADER_PTR = .;
    *(.header);
    *(.text*);
    . = ALIGN(4K);
  }

  .data :
  {
    *(.data*);
    BOOT_LOAD_END_PTR = .;
    . = ALIGN(4K);
  }

  .bss :
  {
    BOOT_BSS_PTR = .;
    *(.bss*);
    *(COMMON);
    BOOT_BSS_END_PTR = .;
    . = ALIGN(4K);
  }
}
