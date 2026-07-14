U := ../../userspace
SYSROOT_STAGE := libc
ULIBS := c
include $(U)/build.mk

C_PUBLIC_HEADERS := $(wildcard *.h)
C_SOURCES := $(wildcard *.c)
C_OBJECTS := $(patsubst %.c,out/%.c.o,$(C_SOURCES))

.PHONY: all install
all: out/gdoslib.a

out/gdoslib.a: $(C_OBJECTS)
	$(AR) rcs $@ $^

install: out/gdoslib.a
	install -d $(DESTDIR)/usr/include $(DESTDIR)/usr/lib
	install -m644 $(C_PUBLIC_HEADERS) $(DESTDIR)/usr/include/
	install -m644 out/gdoslib.a $(DESTDIR)/usr/lib/gdoslib.a

PKG_NAME := gdoslib-dev
PKG_INPUTS := out/gdoslib.a $(C_PUBLIC_HEADERS)
include $(GDOS_ROOT)/mk/package.mk
