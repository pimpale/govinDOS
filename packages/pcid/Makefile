.DEFAULT_GOAL := all

PROGRAM := pcid
SOURCES := $(wildcard *.c)
OBJECTS := $(patsubst %.c,out/%.c.o,$(SOURCES))
LDLIBS := -lgdoslib -lc
LINK_DEPS := $(SYSROOT)/usr/lib/gdoslib.lib $(SYSROOT)/usr/lib/c.lib

.PHONY: all install clean
all: out/$(PROGRAM).exe

$(LINK_DEPS):
	@test -f $@ || { echo "missing target build dependency: $@" >&2; exit 1; }

out/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/$(PROGRAM).exe: $(OBJECTS) $(LINK_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

install: out/$(PROGRAM).exe
	install -Dm755 $< $(DESTDIR)/bin/$(PROGRAM).exe

clean:
	rm -rf out

-include $(OBJECTS:%=%.d)
