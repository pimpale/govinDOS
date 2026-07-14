# Shared toolchain, generated sysroot, flags, and rules for every userspace
# component (docs/technical/package-build.md). Each component sets U (the
# relative path back to userspace/) and may select a SYSROOT_STAGE before
# including this file. Public headers and libraries are consumed from the
# generated sysroot, exactly as future ports will consume them.
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

GDOS_ROOT := $(abspath $(U)/..)
include $(GDOS_ROOT)/mk/apk.mk
SYSROOT := $(GDOS_ROOT)/out/sysroot
SYSROOT_STAGE ?= full
SYSROOT_READY := $(GDOS_ROOT)/out/stamps/sysroot-$(SYSROOT_STAGE)

CFLAGS := \
	-std=c23 \
	-target x86_64-unknown-windows \
	-ffreestanding -fno-builtin -mno-red-zone \
	-mgeneral-regs-only \
	-fno-stack-protector \
	-O1 \
	-I$(SYSROOT)/usr/include

LDFLAGS := \
	-flavor link \
	-subsystem:native \
	-entry:_start \
	-nodefaultlib \
	-fixed:no \
	-dynamicbase \
	-debug:none

# ULIBS names development packages already installed into the sysroot. There
# are no source-tree include paths here: this is the boundary that lets an
# in-tree daemon and a ported library use the same compiler environment.
LIBS := $(foreach l,$(ULIBS),$(SYSROOT)/usr/lib/$(l).a)

# A direct component build can bootstrap a missing sysroot. As before,
# cross-component staleness belongs to the userspace orchestrator; the order-
# only edge keeps an unchanged sysroot stamp from rebuilding every object.
$(SYSROOT_READY):
	$(MAKE) -C $(U) sysroot-$(SYSROOT_STAGE)

$(SYSROOT)/usr/lib/%.a: | $(SYSROOT_READY)
	@test -f $@ || { echo "missing sysroot library: $@" >&2; exit 1; }

out/%.c.o: %.c | $(SYSROOT_READY)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f win64 -o $@ $<

.PHONY: clean
clean:
	rm -rf out

-include $(shell find out -name '*.d' 2>/dev/null)
