# Kernel build. Objects land in out/ mirroring the source tree, named
# <source>.o (e.g. out/src/init.c.o), same scheme as the old make.sh.

CC      := clang-22
NASM    := nasm
LD      := lld-link

CFLAGS := \
	-std=c23 \
	-target x86_64-unknown-windows \
	-ffreestanding -fno-builtin -fshort-wchar -mno-red-zone \
	-mgeneral-regs-only \
	-fstack-protector-all \
	-Wframe-larger-than=8192 \
	-O0 -g \
	-DPRINTF_SUPPORT_DECIMAL_SPECIFIERS=0 \
	-DPRINTF_SUPPORT_EXPONENTIAL_SPECIFIERS=0 \
	-Isrc \
	-Iarchsrc/x86_64 \
	-Ivendor \
	-Isrc/stdlib \
	-Iselftest \
	-I../abi

LDFLAGS := \
	-flavor link \
	-debug \
	-subsystem:efi_application \
	-entry:efi_main

# All kernel C sources, discovered automatically. New .c files are
# picked up on the next make with no Makefile edit. selftest/boot holds
# the boot-time in-kernel selftests; the rest of selftest/ is hosted
# suites that must never enter the image.
CSRCS   := $(shell find src archsrc vendor selftest/boot -name '*.c')

# COFF assembly. Non-recursive on purpose: archsrc/x86_64/blobs/ holds
# flat-binary sources that must NOT be assembled as win64 objects.
ASMSRCS := $(wildcard archsrc/x86_64/*.asm)

OBJS := $(addprefix out/,$(addsuffix .o,$(CSRCS) $(ASMSRCS)))

.PHONY: all clean
all: out/kernel.efi

out/kernel.efi: $(OBJS)
	$(LD) $(LDFLAGS) -out:$@ $^

out/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -g -f win64 -o $@ $<

# --- AP trampoline -----------------------------------------------------------
# Flat-binary assembly: the trampoline must be position-correct at a fixed
# physical address rather than relocatable, so it is built with -f bin and
# embedded into the kernel via an incbin wrapper.
out/archsrc/x86_64/blobs/ap_trampoline.asm.bin: archsrc/x86_64/blobs/ap_trampoline.asm
	@mkdir -p $(dir $@)
	$(NASM) -f bin -o $@ $<

# The wrapper incbin's the flat binary, so rebuild it when the binary changes.
out/archsrc/x86_64/ap_trampoline_blob.asm.o: out/archsrc/x86_64/blobs/ap_trampoline.asm.bin

clean:
	rm -rf out

# Header dependency tracking, emitted by -MMD above.
-include $(OBJS:%=%.d)
