# Common native APK repository and root-composition commands. This file is a
# Make include, not a package recipe; package metadata belongs in APKBUILD.

ifndef GDOS_ROOT
$(error apk.mk requires GDOS_ROOT to name the repository root)
endif

GDOS_ROOT := $(abspath $(GDOS_ROOT))
include $(GDOS_ROOT)/mk/version.mk

APK ?= apk
ABUILD ?= abuild
ABUILD_REPODEST ?= $(GDOS_ROOT)/out/repository
ABUILD_REPOSITORY ?= $(ABUILD_REPODEST)/packages
ABUILD_ARCH_REPOSITORY ?= $(ABUILD_REPOSITORY)/$(GDOS_PACKAGE_ARCH)
ABUILD_INDEX ?= $(ABUILD_ARCH_REPOSITORY)/APKINDEX.tar.gz
APK_KEYS_DIR ?= $(HOME)/.abuild

# Arch Linux's busybox enables standalone applets. abuild itself runs under
# busybox ash, whose internal tar lacks the GNU options abuild requires.
# Executing the fakeroot pass through bash selects /usr/bin/tar. On Alpine this
# remains equivalent to the usual `fakeroot abuild` path.
ABUILD_FAKEROOT ?= fakeroot bash
ABUILD_ENV = CARCH=$(GDOS_PACKAGE_ARCH) FAKEROOT='$(ABUILD_FAKEROOT)'

# Any APKBUILD in the packages repository can ask abuild to index all .apk
# files there. gdos-abi is the stable anchor used by the orchestrator.
ABUILD_INDEX_RECIPE ?= $(GDOS_ROOT)/packages/gdos-abi

define apk_index_repository
	$(ABUILD_ENV) $(ABUILD) -C $(ABUILD_INDEX_RECIPE) \
		-P $(ABUILD_REPODEST) index
endef

# $(call apk_install_root,destination,world packages)
define apk_install_root
	rm -rf $(1)
	mkdir -p $(1)
	$(APK) --root $(1) --arch $(GDOS_PACKAGE_ARCH) \
		--keys-dir $(APK_KEYS_DIR) --repository $(ABUILD_REPOSITORY) \
		add --initdb --usermode $(2)
endef
