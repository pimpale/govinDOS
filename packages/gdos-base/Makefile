GDOS_ROOT := ../..

.PHONY: all install
all: package

# A metapackage has no payload. Its dependency closure defines the default
# system image, just as alpine-base does for an Alpine installation.
install:
	install -d $(DESTDIR)

PKG_NAME := gdos-base
include $(GDOS_ROOT)/mk/package.mk
