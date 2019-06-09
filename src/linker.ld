ENTRY(start)

SECTIONS
{
    /* kernel_phys_offset = 0xffffffffc0000000; */
    kernel_phys_offset = 0x0;
    . = kernel_phys_offset + 0x100000;

    .text ALIGN(4K) :
    {

        header_addr = . - kernel_phys_offset;
        KEEP(*(.multiboot))
        load_addr = . - kernel_phys_offset;
        KEEP(*(.text*))
        load_end_addr = . - kernel_phys_offset;
    }

    .data ALIGN(4K) :
    {
        data_addr = . - kernel_phys_offset;
        KEEP(*(.data*))
        KEEP(*(.rodata*))
        data_end_addr = . - kernel_phys_offset;
    }

    .bss ALIGN(4K) :
    {
        bss_addr = . - kernel_phys_offset;
        KEEP(*(COMMON))
        KEEP(*(.bss*))
        bss_end_addr = . - kernel_phys_offset;
    }
}
