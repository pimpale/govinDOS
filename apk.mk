# Shared APK repository, cross-build root, and installation configuration.

ifndef GDOS_ROOT
$(error apk.mk requires GDOS_ROOT to name the repository root)
endif

GDOS_ROOT := $(abspath $(GDOS_ROOT))
GDOS_PACKAGE_ARCH := x86_64-govindos
GDOS_TARGET_TRIPLET := x86_64-pc-windows-msvc

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
APK_ROOT_ADAPTER := sh $(GDOS_ROOT)/tools/apk-root.sh

# Abuild installs makedepends_host into CBUILDROOT. The root .abuild file puts
# the prefixed clang/lld tools on PATH. BOOTSTRAP=no suppresses Alpine's
# build-base target dependency because GovinDOS supplies its own SDK packages.
ABUILD_FAKEROOT ?= fakeroot bash
GDOS_ABUILD_ENV = \
	CARCH=$(GDOS_PACKAGE_ARCH) \
	CHOST=$(GDOS_TARGET_TRIPLET) \
	CTARGET=$(GDOS_TARGET_TRIPLET) \
	CLIBC=none CTARGET_LIBC=none \
	CBUILDROOT=$(BUILD_ROOT) \
	GDOS_HOST_BUILD_ROOT=$(HOST_BUILD_ROOT) \
	GDOS_APK_KEYS_DIR=$(APK_KEYS_DIR) \
	BOOTSTRAP=no \
	FAKEROOT='$(ABUILD_FAKEROOT)' \
	APK='$(APK_ROOT_ADAPTER)' \
	SUDO_APK='$(APK_ROOT_ADAPTER)'

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
