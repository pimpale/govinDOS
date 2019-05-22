C_SRCS=kernel.c
CC=x86_64-elf-gcc
NASM=nasm 

.PHONY: all

all:
	$(NASM) -felf64 boot.asm -o $(BUILD_DIR)/boot.o
	$(NASM) -felf64 vga_text.asm -o $(BUILD_DIR)/vga_text.o
	$(NASM) -felf64 kernel.asm -o $(BUILD_DIR)/kernel.o

	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/vga_text.o

