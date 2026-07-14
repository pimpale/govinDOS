GDOS_ROOT := ../..
SYSCALL_HEADERS := $(wildcard include/gdos/*.h)

.PHONY: all install
all: package

install:
	install -d $(DESTDIR)/usr/include/gdos
	install -m644 $(SYSCALL_HEADERS) $(DESTDIR)/usr/include/gdos/

PKG_NAME := gdos-syscalls
PKG_INPUTS := $(SYSCALL_HEADERS)
include $(GDOS_ROOT)/mk/package.mk
