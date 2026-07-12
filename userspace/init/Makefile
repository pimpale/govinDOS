U := ..
ULIBS := sys c
include $(U)/build.mk

# The programs packaged into init's initfs, pulled from the sibling
# components' ARTIFACTS (bin/<p>/out/<p>.exe — never their sources).
# This manifest lives here because init is the packager. Programs not
# listed still build; they ship on the filesystem tree instead
# (userspace/Makefile).
INITFS_PROGS := tests

all: out/init.exe

INITFS_FILES := $(foreach p,$(INITFS_PROGS),$(U)/bin/$(p)/out/$(p).exe)

# Member names relative to out/initfs/, no leading "./" (lib/sys/cpio.c
# is the parser). cpio newc.
out/initdata.cpio: $(INITFS_FILES)
	rm -rf out/initfs
	mkdir -p out/initfs
	cp $^ out/initfs/
	cd out/initfs && find . -mindepth 1 ! -type d | sed 's|^\./||' | \
	    LC_ALL=C sort | cpio -o -H newc --quiet > ../initdata.cpio

# The initfs rides inside init.exe as a read-only section; bootfs.asm
# incbin's it and exports bootfs_start/bootfs_end (the whole discovery
# mechanism — the kernel never learns the archive exists).
out/bootfs.asm.o: out/initdata.cpio

out/init.exe: $(patsubst %.c,out/%.c.o,$(wildcard *.c)) out/bootfs.asm.o $(LIBS)
	$(LD) $(LDFLAGS) -out:$@ $^
