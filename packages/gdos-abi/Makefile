GDOS_ROOT := ../..

ABI_HEADERS := $(wildcard $(GDOS_ROOT)/abi/gdosabi/*.h)

.PHONY: all install
all:

install:
	install -d $(DESTDIR)/usr/include/gdosabi
	install -m644 $(ABI_HEADERS) $(DESTDIR)/usr/include/gdosabi/
