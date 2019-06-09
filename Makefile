NAME=govinDOS
ROOT=$(shell pwd)

BUILD_DIR=$(ROOT)/build
SRC_DIR=$(ROOT)/src

.PHONY: all clean src iso run

all: iso

clean:
	rm -rf $(BUILD_DIR)

src: clean
	mkdir -p $(BUILD_DIR)/boot
	$(MAKE) -C src SRC_DIR=$(SRC_DIR) BUILD_DIR=$(BUILD_DIR) ROOT=$(ROOT)


iso: src
	mkdir -p $(BUILD_DIR)/boot/grub
	cp grub.cfg $(BUILD_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/$(NAME).iso $(BUILD_DIR)

run-kernel: src
	qemu-system-x86_64 -monitor stdio -no-shutdown -no-reboot -d int -kernel $(BUILD_DIR)/boot/kernel.bin

run-iso: iso
	qemu-system-x86_64 -monitor stdio -no-shutdown -no-reboot -d int -cdrom $(BUILD_DIR)/$(NAME).iso
