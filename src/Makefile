LIBC=$(ROOT)/third_party/musl
C_SRCS=kernel.c
CC=clang --target=i386-elf
NASM=nasm -felf32

.PHONY: all

all:
	$(NASM) boot.asm -o $(BUILD_DIR)/boot.o

	$(CC) -c $(C_SRCS) -o $(BUILD_DIR)/kernel.o \
	-std=c11 -ffreestanding -O2 -Wall -Wextra
	
	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	-I $(LIBC)/include \
	$(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o \
	-lgcc

