C_SRCS=kernel.c
CC=i686-elf-gcc
NASM=nasm 

.PHONY: all

all:
	$(NASM) -felf32 boot.asm -o $(BUILD_DIR)/boot.o
	$(NASM) -felf32 term.asm -o $(BUILD_DIR)/term.o

	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/boot.o $(BUILD_DIR)/term.o

