# Publish third-party APKBUILDs as the external ports repository.

GDOS_ROOT := $(abspath ..)
include $(GDOS_ROOT)/apk.mk

STAMPS := $(GDOS_ROOT)/out/stamps
REPOSITORY_STAMP := $(STAMPS)/repository-ports
PORT_RECIPES := $(wildcard */APKBUILD)
PORT_INPUTS := Makefile $(GDOS_ROOT)/.abuild $(GDOS_ROOT)/apk.mk \
	$(GDOS_ROOT)/tools/apk-root.sh \
	$(shell find $(GDOS_ROOT)/toolchain -type f) \
	$(shell find . -mindepth 2 -type f \
	! -path './*/src/*' ! -path './*/pkg/*' ! -path './*/tmp/*')

.PHONY: all clean

ifeq ($(strip $(PORT_RECIPES)),)
all:
	@echo "No external ports to build"
else
all: $(REPOSITORY_STAMP)

$(REPOSITORY_STAMP): $(PORT_INPUTS)
	rm -rf $(PORTS_REPOSITORY)
	mkdir -p $(STAMPS)
	$(call apk_initialize_root,$(HOST_BUILD_ROOT),x86_64)
	$(call apk_initialize_root,$(BUILD_ROOT),$(GDOS_PACKAGE_ARCH))
	$(GDOS_ABUILD_ENV) $(BUILDREPO) \
		-a $(GDOS_ROOT) -d $(ABUILD_REPODEST) -p ports
	touch $@
endif

clean:
	rm -rf $(PORTS_REPOSITORY) $(REPOSITORY_STAMP)
	for d in */; do rm -rf "$$d/src" "$$d/pkg" "$$d/tmp"; done
