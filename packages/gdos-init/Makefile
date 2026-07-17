.DEFAULT_GOAL := all

LDLIBS := -lgdoslib -lc
LINK_DEPS := $(SYSROOT)/usr/lib/gdoslib.lib $(SYSROOT)/usr/lib/c.lib
C_SOURCES := $(wildcard *.c)
C_OBJECTS := $(patsubst %.c,out/%.c.o,$(C_SOURCES))
INIT_OBJECTS := $(C_OBJECTS) out/bootfs.asm.o

# abuild installs these declared makedepends_host packages into the target build
# root. init embeds those installed payloads, never sibling build artifacts.
INITFS_PROGS := nvmed pcid tests
INITFS_PROGRAMS := $(foreach p,$(INITFS_PROGS),$(SYSROOT)/bin/$(p).exe)

all: out/init.exe

$(LINK_DEPS):
	@test -f $@ || { echo "missing target build dependency: $@" >&2; exit 1; }

out/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $@.d -c -o $@ $<

out/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

# Member names relative to out/initfs/, no leading "./" (gdoslib-dev/cpio.c
# is the parser). cpio newc.
# Package payloads carry reproducible mtimes, so timestamp comparison cannot
# tell whether buildrepo just installed a newer executable into the sysroot.
# Refresh the small embedded archive whenever gdos-init itself is built.
.PHONY: FORCE
FORCE:

out/initdata.cpio: FORCE $(INITFS_PROGRAMS)
	rm -rf out/initfs
	mkdir -p out/initfs
	cp $(INITFS_PROGRAMS) out/initfs/
	cd out/initfs && find . -mindepth 1 ! -type d | sed 's|^\./||' | \
	    LC_ALL=C sort | cpio -o -H newc --quiet > ../initdata.cpio

# The initfs rides inside init.exe as a read-only section; bootfs.asm
# incbin's it and exports bootfs_start/bootfs_end (the whole discovery
# mechanism — the kernel never learns the archive exists).
out/bootfs.asm.o: out/initdata.cpio

out/init.exe: $(INIT_OBJECTS) $(LINK_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(INIT_OBJECTS) $(LDLIBS)

.PHONY: install clean
install: out/init.exe
	install -Dm755 $< $(DESTDIR)/boot/init.exe

clean:
	rm -rf out

-include $(C_OBJECTS:%=%.d)
