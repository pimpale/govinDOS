# Publish the first-party APKBUILDs as the native binary repository.

GDOS_ROOT := $(abspath ..)
include $(GDOS_ROOT)/apk.mk

STAMPS := $(GDOS_ROOT)/out/stamps
REPOSITORY_STAMP := $(STAMPS)/repository-packages

# Buildrepo considers an existing versioned APK current. First-party recipes
# build live monorepo source, so any source or toolchain change invalidates the
# small native repository as a unit.
PACKAGE_INPUTS := Makefile $(GDOS_ROOT)/.abuild $(GDOS_ROOT)/apk.mk \
	$(GDOS_ROOT)/tools/apk-root.sh \
	$(shell find $(GDOS_ROOT)/toolchain -type f) \
	$(shell find . -mindepth 2 -type f \
	! -path './*/out/*' ! -path './*/src/*' \
	! -path './*/pkg/*' ! -path './*/tmp/*') \
	$(wildcard $(GDOS_ROOT)/abi/gdosabi/*.h)

.PHONY: all clean

all: $(REPOSITORY_STAMP)

$(REPOSITORY_STAMP): $(PACKAGE_INPUTS)
	rm -rf $(NATIVE_REPOSITORY)
	mkdir -p $(STAMPS)
	$(call apk_initialize_root,$(HOST_BUILD_ROOT),x86_64)
	$(call apk_initialize_root,$(BUILD_ROOT),$(GDOS_PACKAGE_ARCH))
	$(GDOS_ABUILD_ENV) $(BUILDREPO) \
		-a $(GDOS_ROOT) -d $(ABUILD_REPODEST) -p packages
	touch $@

clean:
	rm -rf $(NATIVE_REPOSITORY) $(HOST_BUILD_ROOT) $(BUILD_ROOT) \
		$(REPOSITORY_STAMP)
	for d in */; do \
		rm -rf "$$d/out" "$$d/src" "$$d/pkg" "$$d/tmp"; \
	done
