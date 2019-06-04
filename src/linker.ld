ENTRY(start)
phys_addr = 0x00100000;
SECTIONS
{
  .text phys_addr : AT(phys_addr) 
  {
    header_addr = .;
    *(.multiboot)
    load_addr = .;
    *(.text)
    load_end_addr = .;
    *(.rodata)
    . = ALIGN(4096);
  }
  .data : AT(phys_addr + (data_addr - load_end_addr))
  {
    data_addr = .;
    *(.data)
    . = ALIGN(4096);
  }
  .bss : AT(phys_addr + (bss_addr - load_addr))
  {
    bss_addr = .;
    *(.bss)
    . = ALIGN(4096);
  }
  bss_end_addr = .;
}
