# Uniform first-party program build and package staging.  The including
# Makefile sets U, PROGRAM, and ULIBS; optional PKG_* metadata may be set before
# inclusion.  Programs are installed under /bin to preserve the current image
# contract.

ifndef PROGRAM
$(error program.mk requires PROGRAM)
endif

include $(U)/build.mk

C_SOURCES := $(wildcard *.c)
C_OBJECTS := $(patsubst %.c,out/%.c.o,$(C_SOURCES))
PROGRAM_ARTIFACT := out/$(PROGRAM).exe

.PHONY: all install
all: $(PROGRAM_ARTIFACT)

$(PROGRAM_ARTIFACT): $(C_OBJECTS) $(LIBS)
	$(LD) $(LDFLAGS) -out:$@ $^

install: $(PROGRAM_ARTIFACT)
	install -Dm755 $< $(DESTDIR)/bin/$(PROGRAM).exe

PKG_NAME ?= $(PROGRAM)
PKG_INPUTS := $(PROGRAM_ARTIFACT)
include $(GDOS_ROOT)/mk/package.mk
