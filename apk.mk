# Shared APK repository, cross-build root, and installation configuration.

ifndef GDOS_ROOT
$(error apk.mk requires GDOS_ROOT to name the repository root)
endif

GDOS_ROOT := $(abspath $(GDOS_ROOT))
GDOS_PACKAGE_ARCH := x86_64-govindos

APK ?= apk
BUILDREPO ?= buildrepo
ABUILD_REPODEST ?= $(GDOS_ROOT)/out/repository
NATIVE_REPOSITORY := $(ABUILD_REPODEST)/packages
PORTS_REPOSITORY := $(ABUILD_REPODEST)/ports
PORTS_INDEX := $(PORTS_REPOSITORY)/$(GDOS_PACKAGE_ARCH)/APKINDEX.tar.gz
APK_KEYS_DIR ?= $(HOME)/.abuild

BUILD_ROOT ?= $(GDOS_ROOT)/out/buildroot
HOST_BUILD_ROOT ?= $(GDOS_ROOT)/out/host-buildroot
SYSROOT ?= $(GDOS_ROOT)/out/sysroot

# Run a command from the repository root in the same abuild environment used
# interactively by bash, zsh, and fish users. The trailing && lets callers append
# the command and arguments in their recipe.
WITH_ABUILD_ENV = cd "$(GDOS_ROOT)" && . ./abuild.env &&

APK_REPOSITORY_ARGS = --repository $(NATIVE_REPOSITORY) \
	$(if $(wildcard $(PORTS_INDEX)),--repository $(PORTS_REPOSITORY))

# $(call apk_initialize_root,destination,architecture)
define apk_initialize_root
	rm -rf $(1)
	mkdir -p $(1)
	$(APK) --root $(1) --arch $(2) add --initdb --usermode
endef

# $(call apk_install_root,destination,world packages)
define apk_install_root
	rm -rf $(1)
	mkdir -p $(1)
	$(APK) --root $(1) --arch $(GDOS_PACKAGE_ARCH) \
		--keys-dir $(APK_KEYS_DIR) $(APK_REPOSITORY_ARGS) \
		add --initdb --usermode $(2)
endef
