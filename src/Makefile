CC=i686-elf-gcc
NASM=nasm 
NASMFLAGS=-felf32

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) multiboot.asm -o $(BUILD_DIR)/multiboot.o
	$(NASM) $(NASMFLAGS) start.asm -o $(BUILD_DIR)/start.o
	$(NASM) $(NASMFLAGS) long.asm -o $(BUILD_DIR)/long.o
	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/multiboot.o \
	$(BUILD_DIR)/long.o \
	$(BUILD_DIR)/start.o
