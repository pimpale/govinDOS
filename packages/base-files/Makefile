GDOS_ROOT := ../..
BASE_FILES := $(shell find root -type f)

.PHONY: all install
all: package

install:
	install -d $(DESTDIR)
	cp -a --no-preserve=ownership root/. $(DESTDIR)/

PKG_NAME := base-files
PKG_INPUTS := $(BASE_FILES)
include $(GDOS_ROOT)/mk/package.mk
