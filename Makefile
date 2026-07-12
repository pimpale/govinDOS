# Top-level build: builds the kernel, stages the EFI boot tree in out/root,
# and can boot the result in qemu (`make runkernel`).

.PHONY: all kernel userspace clean cleanall runkernel

all: kernel userspace
	mkdir -p out out/root
	cp -rT efi out/efi
	mkdir -p out/root/EFI/BOOT out/root/boot
	cp kernel/out/kernel.efi out/root/EFI/BOOT/BOOTX64.efi
	cp userspace/out/init.exe out/root/boot/init.exe
	# The userspace filesystem tree, merged onto the ESP for now — EFI/
	# and boot/ don't collide with the unix dirs. Moves to its own
	# partition once a real filesystem exists.
	cp -rT userspace/out/fs out/root

# Always recurse; the sub-Makefiles decide what is out of date.
kernel:
	$(MAKE) -C kernel

userspace:
	$(MAKE) -C userspace

# Add -d int to the qemu invocation to trace interrupts.
runkernel: all
	qemu-system-x86_64 \
	  -no-reboot \
	  -cpu max \
	  -drive if=pflash,format=raw,file=./out/efi/OVMF.fd \
	  -drive format=raw,file=fat:rw:out/root \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	  -serial stdio \
	  -m 1G \
	  -smp 4 \
	  -net none

clean:
	rm -rf out

cleanall: clean
	$(MAKE) -C kernel clean
	$(MAKE) -C userspace clean
