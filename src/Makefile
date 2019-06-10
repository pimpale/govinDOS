CC = i686-elf-gcc
CCFLAGS = -m32 -mcmodel=large -fno-pic -ffreestanding -nostdlib 
NASM = nasm 
NASMFLAGS = -f elf32

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) multiboot.asm -o $(BUILD_DIR)/multiboot.o
	$(NASM) $(NASMFLAGS) vga.asm -o $(BUILD_DIR)/vga.o
	$(NASM) $(NASMFLAGS) start32.asm -o $(BUILD_DIR)/start32.o
	$(NASM) $(NASMFLAGS) 32.asm -o $(BUILD_DIR)/32.o
	$(CC) $(CCFLAGS) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin $(BUILD_DIR)/*.o

