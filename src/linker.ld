ENTRY(start)

SECTIONS
{
  /* Where the kernel will be mapped to */
  KERNEL_VIRT_ADDR = 0xFFFFFFFFC0000000; 
  /* Where the kernel physically is in memory */
  KERNEL_PHYS_ADDR = 0x10000;

  /* We start 1M away */
  . += KERNEL_PHYS_ADDR;
  /* Shove all the boot + paging stuff over here */
  .boot : {
    *(.boot.header);
    *(.boot.text);
    *(.boot.data);
    . = ALIGN(4K);
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
