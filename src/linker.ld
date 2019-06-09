ENTRY(start)

SECTIONS
{
  /* KERNEL_VIRT_ADDR = 0xFFFFFFFFC0000000; */ 
  KERNEL_VIRT_ADDR = 0x10;
  . = 0x100000;

  .multiboot ALIGN(4K) : {
    *(.multiboot);
    . = ALIGN(4K);
}

. += KERNEL_VIRT_ADDR;

.text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_ADDR)
{
  *(.text*);
  . = ALIGN(4K);
}

.data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_ADDR)
{
  *(.data*);
  . = ALIGN(4K);
}

.bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRT_ADDR)
{
  *(.bss*);
  *(COMMON);
  . = ALIGN(4K);
}
}
