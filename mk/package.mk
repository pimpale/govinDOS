# Bridge a source component's Make targets to its native APKBUILD recipe.
# The including Makefile supplies PKG_NAME and PKG_INPUTS. Package metadata,
# dependencies, build phases, and installation policy live only in APKBUILD.

ifndef GDOS_ROOT
$(error package.mk requires GDOS_ROOT to name the repository root)
endif
ifndef PKG_NAME
$(error package.mk requires PKG_NAME)
endif

GDOS_ROOT := $(abspath $(GDOS_ROOT))
include $(GDOS_ROOT)/mk/apk.mk

PKG_INPUTS ?=
PKG_RECIPE ?= $(GDOS_ROOT)/packages/$(PKG_NAME)
PKG_APKBUILD := $(PKG_RECIPE)/APKBUILD
PKG_FILENAME := $(strip $(shell CARCH=$(GDOS_PACKAGE_ARCH) \
	$(ABUILD) -q -C $(PKG_RECIPE) -P $(ABUILD_REPODEST) listpkg))
PKG_FILE := $(ABUILD_ARCH_REPOSITORY)/$(PKG_FILENAME)

.PHONY: package
package: $(PKG_FILE)

$(PKG_FILE): $(PKG_INPUTS) $(PKG_APKBUILD) \
		$(GDOS_ROOT)/mk/package.mk $(GDOS_ROOT)/mk/apk.mk \
		$(GDOS_ROOT)/mk/version.mk
	$(ABUILD_ENV) $(ABUILD) -C $(PKG_RECIPE) \
		-P $(ABUILD_REPODEST) -d -f rootpkg
	$(ABUILD_ENV) $(ABUILD) -C $(PKG_RECIPE) \
		-P $(ABUILD_REPODEST) clean
	test -f $@
