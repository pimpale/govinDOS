.DEFAULT_GOAL := all

CFLAGS += \
	-I./source \
	-I./vendor \
	-DPRINTF_SUPPORT_DECIMAL_SPECIFIERS=0 \
	-DPRINTF_SUPPORT_EXPONENTIAL_SPECIFIERS=0

C_PUBLIC_HEADERS := \
	source/assert.h \
	source/ctype.h \
	source/errno.h \
	source/limits.h \
	source/math.h \
	source/pthread.h \
	source/sched.h \
	source/stdio.h \
	source/stdlib.h \
	source/string.h \
	source/strings.h \
	source/time.h \
	source/unistd.h \
	vendor/printf/printf.h
C_SOURCES := \
	source/ctype.c \
	source/errno.c \
	source/pthread.c \
	source/stdio.c \
	source/stdlib.c \
	source/string.c \
	source/tls.c \
	source/unistd.c \
	vendor/printf/printf.c
C_OBJECTS := $(patsubst %.c,out/%.c.o,$(C_SOURCES))

.PHONY: all install clean
all: out/c.lib

out/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/c.lib: $(C_OBJECTS)
	$(AR) $(ARFLAGS) $@ $^

install: out/c.lib
	install -d $(DESTDIR)/usr/include $(DESTDIR)/usr/include/printf \
		$(DESTDIR)/usr/lib
	install -m644 $(filter source/%.h,$(C_PUBLIC_HEADERS)) \
		$(DESTDIR)/usr/include/
	install -m644 vendor/printf/printf.h $(DESTDIR)/usr/include/printf/
	install -m644 out/c.lib $(DESTDIR)/usr/lib/c.lib

clean:
	rm -rf out

-include $(C_OBJECTS:%=%.d)
