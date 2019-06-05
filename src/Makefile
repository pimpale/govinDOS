CC=i686-elf-gcc
NASM=nasm 
NASMFLAGS=-felf32

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) multiboot.asm -o $(BUILD_DIR)/multiboot.o
	$(NASM) $(NASMFLAGS) vga.asm -o $(BUILD_DIR)/vga.o
	$(NASM) $(NASMFLAGS) start32.asm -o $(BUILD_DIR)/start32.o
	$(NASM) $(NASMFLAGS) 32.asm -o $(BUILD_DIR)/32.o
	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/multiboot.o \
	$(BUILD_DIR)/vga.o \
	$(BUILD_DIR)/start32.o \
	$(BUILD_DIR)/32.o \

