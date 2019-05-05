C_SRCS=kernel.c
CC=i686-elf-gcc
NASM=nasm -felf32

.PHONY: all

all:
	$(NASM) boot.asm -o $(BUILD_DIR)/boot.o

	$(CC) -c $(C_SRCS) -o $(BUILD_DIR)/kernel.o \
	-std=c11 -ffreestanding -O2 -Wall -Wextra
	
	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o

