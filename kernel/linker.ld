OUTPUT_FORMAT("binary")
ENTRY(early_init)
SECTIONS
{
  /* Where the kernel physically is in memory */
  KERNEL_PHYS = 0x10000;

  /* We start 1M away */
  . += KERNEL_PHYS;

  .text :
  {
    BOOT_LOAD = .;
    BOOT_HEADER = .;
    *(.header);
    *(.text*);
    . = ALIGN(4K);
  }

  .data :
  {
    *(.data*);
    BOOT_LOAD_END = .;
    . = ALIGN(4K);
  }

  .bss :
  {
    BOOT_BSS = .;
    *(.bss*);
    *(COMMON);
    BOOT_BSS_END = .;
    . = ALIGN(4K);
  }
}
