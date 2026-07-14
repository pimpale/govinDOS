# Userspace package orchestrator (docs/technical/package-build.md).
#
# Source components own compilation and DESTDIR installation. This file owns
# only dependency order and the three build-time products shared across them:
#
#   ../out/repository/ signed .apk packages + APKINDEX
#   ../out/sysroot/    development-package closure used by clang/clangd
#   ../out/image-root/ runtime-package closure selected by an image profile

GDOS_ROOT := $(abspath ..)
OUT := $(GDOS_ROOT)/out
SYSROOT := $(OUT)/sysroot
STAMPS := $(OUT)/stamps
MANIFESTS := $(OUT)/manifests
include $(GDOS_ROOT)/mk/apk.mk

PROGS := nvmed pcid tests
PROGRAM_PACKAGE_TARGETS := $(addprefix package-program-,$(PROGS))

IMAGE ?= development
IMAGE_PROFILE := $(GDOS_ROOT)/images/$(IMAGE).packages
IMAGE_PACKAGES = $(shell sed -e 's/#.*//' -e '/^[[:space:]]*$$/d' $(IMAGE_PROFILE))

.PHONY: all image packages sysroot sysroot-sdk sysroot-libc sysroot-full \
	package-gdos-abi package-gdos-syscalls package-libc package-gdoslib package-base-files \
	package-init package-gdos-base repository-sdk repository-libc \
	repository-full repository-programs repository-runtime \
	$(PROGRAM_PACKAGE_TARGETS) clean

all: image

# --- Build sysroot ----------------------------------------------------------
# Bootstrap the public ABI/syscall headers, then libc, then gdoslib. Each stage assembles
# the declared development package and its dependency closure. Ports slot into
# this graph by producing another *-dev package before a consumer is compiled.

package-gdos-abi:
	$(MAKE) -C $(GDOS_ROOT)/packages/gdos-abi package

package-gdos-syscalls:
	$(MAKE) -C $(GDOS_ROOT)/packages/gdos-syscalls package

repository-sdk: package-gdos-abi package-gdos-syscalls
	$(apk_index_repository)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/repository-sdk

sysroot-sdk: repository-sdk
	$(call apk_install_root,$(SYSROOT),gdos-syscalls)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/sysroot-sdk

package-libc: sysroot-sdk
	$(MAKE) -C $(GDOS_ROOT)/packages/gdos-libc-dev package

repository-libc: package-libc
	$(apk_index_repository)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/repository-libc

sysroot-libc: repository-libc
	$(call apk_install_root,$(SYSROOT),gdos-libc-dev)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/sysroot-libc

package-gdoslib: sysroot-libc
	$(MAKE) -C $(GDOS_ROOT)/packages/gdoslib-dev package

repository-full: package-gdoslib
	$(apk_index_repository)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/repository-full

sysroot-full: repository-full
	$(call apk_install_root,$(SYSROOT),gdoslib-dev)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/sysroot-full

sysroot: sysroot-full

# --- Runtime packages -------------------------------------------------------

$(PROGRAM_PACKAGE_TARGETS): sysroot-full
	$(MAKE) -C $(GDOS_ROOT)/packages/$(patsubst package-program-%,%,$@) package

repository-programs: $(PROGRAM_PACKAGE_TARGETS)
	$(apk_index_repository)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/repository-programs

package-init: repository-programs
	$(MAKE) -C $(GDOS_ROOT)/packages/gdos-init package

package-base-files:
	$(MAKE) -C $(GDOS_ROOT)/packages/base-files package

package-gdos-base: package-base-files package-init
	$(MAKE) -C $(GDOS_ROOT)/packages/gdos-base package

repository-runtime: package-gdos-base
	$(apk_index_repository)
	mkdir -p $(STAMPS)
	touch $(STAMPS)/repository-runtime

packages: repository-runtime

# APK's solver selects the image closure and APK itself enforces file
# ownership, dependency, and signature rules.
image: packages $(IMAGE_PROFILE)
	$(call apk_install_root,$(OUT)/image-root,$(IMAGE_PACKAGES))
	$(APK) --root $(OUT)/image-root --arch $(GDOS_PACKAGE_ARCH) \
		query --format yaml --installed --fields name,version '*' \
		> $(OUT)/image-root.packages

clean:
	rm -rf $(ABUILD_REPODEST) $(SYSROOT) $(STAMPS) $(MANIFESTS) \
		$(OUT)/image-root $(OUT)/image-root.packages $(CURDIR)/out
	for d in gdos-libc-dev gdoslib-dev $(PROGS) gdos-init; do \
		$(MAKE) -C $(GDOS_ROOT)/packages/$$d clean; \
	done
