.DEFAULT_GOAL := all

C_PUBLIC_HEADERS := $(wildcard *.h)
C_SOURCES := $(wildcard *.c)
C_OBJECTS := $(patsubst %.c,out/%.c.o,$(C_SOURCES))

.PHONY: all install clean
all: out/gdoslib.lib

out/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/gdoslib.lib: $(C_OBJECTS)
	$(AR) $(ARFLAGS) $@ $^

install: out/gdoslib.lib
	install -d $(DESTDIR)/usr/include $(DESTDIR)/usr/lib
	install -m644 $(C_PUBLIC_HEADERS) $(DESTDIR)/usr/include/
	install -m644 out/gdoslib.lib $(DESTDIR)/usr/lib/gdoslib.lib

clean:
	rm -rf out

-include $(C_OBJECTS:%=%.d)
