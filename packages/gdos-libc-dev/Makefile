U := ../../userspace
SYSROOT_STAGE := sdk
include $(U)/build.mk

CFLAGS += \
	-I./source \
	-I./vendor \
	-DPRINTF_SUPPORT_DECIMAL_SPECIFIERS=0 \
	-DPRINTF_SUPPORT_EXPONENTIAL_SPECIFIERS=0

C_PUBLIC_HEADERS := \
	source/assert.h \
	source/math.h \
	source/stdio.h \
	source/stdlib.h \
	source/string.h \
	vendor/printf/printf.h
C_SOURCES := \
	source/stdio.c \
	source/stdlib.c \
	source/string.c \
	vendor/printf/printf.c
C_OBJECTS := $(patsubst %.c,out/%.c.o,$(C_SOURCES))

.PHONY: all install
all: out/c.a

out/c.a: $(C_OBJECTS)
	$(AR) rcs $@ $^

install: out/c.a
	install -d $(DESTDIR)/usr/include $(DESTDIR)/usr/include/printf \
		$(DESTDIR)/usr/lib
	install -m644 $(filter source/%.h,$(C_PUBLIC_HEADERS)) \
		$(DESTDIR)/usr/include/
	install -m644 vendor/printf/printf.h $(DESTDIR)/usr/include/printf/
	install -m644 out/c.a $(DESTDIR)/usr/lib/c.a

PKG_NAME := gdos-libc-dev
PKG_INPUTS := out/c.a $(C_PUBLIC_HEADERS)
include $(GDOS_ROOT)/mk/package.mk
