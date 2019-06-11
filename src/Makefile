CC = x86_64-elf-gcc
CCFLAGS = -mcmodel=large -ffreestanding -nostdlib
NASM = nasm 
NASMFLAGS = -f elf64

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) header32.asm -o $(BUILD_DIR)/header32.o
	$(NASM) $(NASMFLAGS) vga.asm -o $(BUILD_DIR)/vga.o
	$(NASM) $(NASMFLAGS) start32.asm -o $(BUILD_DIR)/start32.o
	$(NASM) $(NASMFLAGS) 32.asm -o $(BUILD_DIR)/32.o
	$(CC) $(CCFLAGS) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin $(BUILD_DIR)/*.o

