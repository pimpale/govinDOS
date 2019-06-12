CC = gcc # since we're only using it for linking, we dont care what kind of compiler it is. TODO use ld or other hand written compiler for simplicity
CCFLAGS = -mcmodel=large -ffreestanding -nostdlib
NASM = nasm 
NASMFLAGS = -f elf64

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) header32.asm -o $(BUILD_DIR)/header32.o
	$(NASM) $(NASMFLAGS) vga.asm -o $(BUILD_DIR)/vga.o
	$(NASM) $(NASMFLAGS) start32.asm -o $(BUILD_DIR)/start32.o
	$(CC) $(CCFLAGS) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin $(BUILD_DIR)/*.o

