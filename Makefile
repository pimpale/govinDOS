# Top-level build: builds the kernel, asks the userspace package graph to
# assemble out/image-root, stages the EFI tree in out/root, and can boot the
# result in qemu (`make runkernel`).

.PHONY: all kernel userspace clean cleanall runkernel compdb

COMPDB ?= compile_commands.json

all: kernel userspace
	rm -rf out/root
	mkdir -p out out/root
	cp -rT efi out/efi
	cp -a out/image-root/. out/root/
	mkdir -p out/root/EFI/BOOT out/root/boot
	cp kernel/out/kernel.efi out/root/EFI/BOOT/BOOTX64.efi
	# The package-selected filesystem is merged onto the ESP for now. It moves
	# to its own partition once the filesystem service is persistent.

# Always recurse; the sub-Makefiles decide what is out of date.
kernel:
	$(MAKE) -C kernel

userspace:
	$(MAKE) -C userspace

# Optional clangd database refresh. Bear observes the real compiler commands,
# including the generated sysroot path and target ABI flags. Clean only the
# compile-producing subprojects so recursive make does not inherit `-B` and
# rebuild every sysroot stage once per component.
compdb:
	$(MAKE) -C kernel clean
	$(MAKE) -C userspace clean
	bear --output $(COMPDB) -- $(MAKE) kernel userspace

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
	$(MAKE) -C userspace clean
