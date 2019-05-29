C_SRCS=kernel.c
CC=x86_64-elf-gcc
NASM=nasm 

.PHONY: all

all:
	$(NASM) -felf64 multiboot.asm -o $(BUILD_DIR)/multiboot.o
	$(NASM) -felf64 init.asm -o $(BUILD_DIR)/init.o
	$(NASM) -felf64 vga_text.asm -o $(BUILD_DIR)/vga_text.o
	#$(NASM) -felf64 kernel.asm -o $(BUILD_DIR)/kernel.o

	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/multiboot.o $(BUILD_DIR)/init.o $(BUILD_DIR)/vga_text.o

