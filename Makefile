NAME=myos
ROOT=$(shell pwd)
BUILD_DIR=$(ROOT)/build

.PHONY: all clean src iso run

all: iso

clean:
	rm -rf $(BUILD_DIR)

src: clean
	mkdir -p $(BUILD_DIR)/boot
	$(MAKE) -C src BUILD_DIR=$(BUILD_DIR) ROOT=$(ROOT)


iso: src
	mkdir -p $(BUILD_DIR)/boot/grub
	cp grub.cfg $(BUILD_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/$(NAME).iso $(BUILD_DIR)

run: iso
	qemu-system-x86_64 -kernel $(BUILD_DIR)/boot/kernel.bin
	#qemu-system-i386 -cdrom $(BUILD_DIR)/$(NAME).iso
