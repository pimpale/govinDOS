SYSCALL_HEADERS := $(wildcard include/gdos/*.h)

.PHONY: all install
all:

install:
	install -d $(DESTDIR)/usr/include/gdos
	install -m644 $(SYSCALL_HEADERS) $(DESTDIR)/usr/include/gdos/
