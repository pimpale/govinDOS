CC=i686-elf-gcc
NASM=nasm 
NASMFLAGS=-felf32

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) boot.asm -o $(BUILD_DIR)/boot.o
	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/boot.o
