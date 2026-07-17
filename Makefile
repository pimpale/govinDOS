# Top-level build: publishes the native APK repository, installs the development
# and runtime worlds, stages the EFI tree, and can boot it with `make runkernel`.

.PHONY: all kernel clean cleanall runkernel compdb

GDOS_ROOT := $(CURDIR)
include apk.mk

OUT := $(GDOS_ROOT)/out
COMPDB ?= compile_commands.json
IMAGE ?= development
IMAGE_PROFILE := $(GDOS_ROOT)/images/$(IMAGE).packages
IMAGE_PACKAGES = $(shell sed -e 's/#.*//' -e '/^[[:space:]]*$$/d' $(IMAGE_PROFILE))

all: kernel
	$(MAKE) -C packages
	$(call apk_install_root,$(SYSROOT),gdoslib-dev)
	$(call apk_install_root,$(OUT)/image-root,$(IMAGE_PACKAGES))
	$(APK) --root $(OUT)/image-root --arch $(GDOS_PACKAGE_ARCH) \
		query --format yaml --installed --fields name,version '*' \
		> $(OUT)/image-root.packages
	rm -rf $(OUT)/root
	mkdir -p $(OUT) $(OUT)/root
	cp -rT efi $(OUT)/efi
	cp -a $(OUT)/image-root/. $(OUT)/root/
	mkdir -p $(OUT)/root/EFI/BOOT $(OUT)/root/boot
	cp kernel/out/kernel.efi $(OUT)/root/EFI/BOOT/BOOTX64.efi
	# The package-selected filesystem is merged onto the ESP for now. It moves
	# to its own partition once the filesystem service is persistent.

# Always recurse; the sub-Makefiles decide what is out of date.
kernel:
	$(MAKE) -C kernel

# Optional clangd database refresh. Bear observes the real compiler commands,
# including the target ABI flags supplied by the repository-local .abuild.
compdb:
	$(MAKE) -C kernel clean
	$(MAKE) -C packages clean
	bear --output $(COMPDB) -- $(MAKE)

# Add -d int to the qemu invocation to trace interrupts.
runkernel: all out/nvme.img
	qemu-system-x86_64 \
	  -machine q35 \
	  -no-reboot \
	  -cpu max \
	  -drive if=pflash,format=raw,file=./out/efi/OVMF.fd \
	  -drive format=raw,file=fat:rw:out/root \
	  -drive id=nvme0,if=none,format=raw,file=out/nvme.img \
	  -device nvme,drive=nvme0,serial=govindos \
	  -device intel-iommu,intremap=off \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	  -serial stdio \
	  -m 1G \
	  -smp 4 \
	  -net none

out/nvme.img:
	mkdir -p out
	truncate -s 64M $@

clean:
	rm -rf out

cleanall: clean
	$(MAKE) -C kernel clean
	$(MAKE) -C packages clean
	$(MAKE) -C ports clean
