BASE_FILES := $(shell find root -type f)

.PHONY: all install
all:

install:
	install -d $(DESTDIR)
	cp -a --no-preserve=ownership root/. $(DESTDIR)/
