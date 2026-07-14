GDOS_ROOT := ../..

ABI_HEADERS := $(wildcard $(GDOS_ROOT)/abi/gdosabi/*.h)

.PHONY: all install
all: package

install:
	install -d $(DESTDIR)/usr/include/gdosabi
	install -m644 $(ABI_HEADERS) $(DESTDIR)/usr/include/gdosabi/

PKG_NAME := gdos-abi
PKG_INPUTS := $(ABI_HEADERS)
include $(GDOS_ROOT)/mk/package.mk
