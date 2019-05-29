CC=x86_64-elf-gcc
NASM=nasm 
NASMFLAGS=-felf64

.PHONY: all

all:
	$(NASM) $(NASMFLAGS) multiboot.asm -o $(BUILD_DIR)/multiboot.o
	$(NASM) $(NASMFLAGS) checks.asm -o $(BUILD_DIR)/checks.o
	$(NASM) $(NASMFLAGS) gdt.asm -o $(BUILD_DIR)/gdt.o
	$(NASM) $(NASMFLAGS) screen.asm -o $(BUILD_DIR)/screen.o
	$(NASM) $(NASMFLAGS) paging.asm -o $(BUILD_DIR)/paging.o
	$(NASM) $(NASMFLAGS) startup.asm -o $(BUILD_DIR)/startup.o
	$(CC) -T linker.ld -o $(BUILD_DIR)/boot/kernel.bin \
	-ffreestanding -shared -O2 -nostdlib \
	$(BUILD_DIR)/multiboot.o \
	$(BUILD_DIR)/checks.o \
	$(BUILD_DIR)/gdt.o \
	$(BUILD_DIR)/screen.o \
	$(BUILD_DIR)/paging.o \
	$(BUILD_DIR)/startup.o
