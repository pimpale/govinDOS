# Shared toolchain, flags, and rules for every userspace component
# (docs/technical/source-tree.md). Each component directory has its own
# Makefile that owns building that directory into its own out/; those
# Makefiles set U (the relative path back to userspace/) and include
# this file. Nothing here defines what a component builds — only how.
#
# Same toolchain as the kernel. -fixed:no keeps the .reloc section:
# every process loads at a fresh address in the single AS, so loaders
# must be able to rebase. -mgeneral-regs-only because the kernel
# preserves x87/SSE across switches (fxsave/fxrstor) but not AVX — user
# code must stay off YMM+ state until XSAVE lands.

.DEFAULT_GOAL := all

CC   := clang-22
LD   := lld-link
AR   := llvm-ar
NASM := nasm

CFLAGS := \
	-std=c23 \
	-target x86_64-unknown-windows \
	-ffreestanding -fno-builtin -mno-red-zone \
	-mgeneral-regs-only \
	-fno-stack-protector \
	-O1 \
	-I$(U)/../abi \
	-I$(U)/lib/sys \
	-I$(U)/lib/c

LDFLAGS := \
	-flavor link \
	-subsystem:native \
	-entry:_start \
	-nodefaultlib \
	-fixed:no \
	-dynamicbase \
	-debug:none

# The libraries, as prerequisites for program links. The rules below
# build a lib if it's missing so a component can be built standalone
# from a clean tree; staleness across components is the orchestrator's
# job (userspace/Makefile builds in dependency order).
LIBS := $(U)/lib/sys/out/sys.a $(U)/lib/c/out/c.a

$(U)/lib/sys/out/sys.a:
	$(MAKE) -C $(U)/lib/sys

$(U)/lib/c/out/c.a:
	$(MAKE) -C $(U)/lib/c

out/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f win64 -o $@ $<

.PHONY: clean
clean:
	rm -rf out

-include $(shell find out -name '*.d' 2>/dev/null)
