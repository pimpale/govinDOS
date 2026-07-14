U := ../../userspace
ULIBS := gdoslib c
include $(U)/build.mk

# The programs packaged into init's initfs are installed from the signed local
# APK repository, never copied from sibling source/build directories.
INITFS_PROGS := nvmed pcid tests
PROGRAM_REPOSITORY_READY := $(GDOS_ROOT)/out/stamps/repository-programs

all: out/init.exe

$(PROGRAM_REPOSITORY_READY):
	$(MAKE) -C $(U) repository-programs

# Member names relative to out/initfs/, no leading "./" (gdoslib-dev/cpio.c
# is the parser). cpio newc.
out/initdata.cpio: $(PROGRAM_REPOSITORY_READY)
	$(call apk_install_root,out/initfs-root,$(INITFS_PROGS))
	rm -rf out/initfs
	mkdir -p out/initfs
	cp $(foreach p,$(INITFS_PROGS),out/initfs-root/bin/$(p).exe) out/initfs/
	cd out/initfs && find . -mindepth 1 ! -type d | sed 's|^\./||' | \
	    LC_ALL=C sort | cpio -o -H newc --quiet > ../initdata.cpio

# The initfs rides inside init.exe as a read-only section; bootfs.asm
# incbin's it and exports bootfs_start/bootfs_end (the whole discovery
# mechanism — the kernel never learns the archive exists).
out/bootfs.asm.o: out/initdata.cpio

out/init.exe: $(patsubst %.c,out/%.c.o,$(wildcard *.c)) out/bootfs.asm.o $(LIBS)
	$(LD) $(LDFLAGS) -out:$@ $^

.PHONY: install
install: out/init.exe
	install -Dm755 $< $(DESTDIR)/boot/init.exe

PKG_NAME := gdos-init
PKG_INPUTS := out/init.exe
include $(GDOS_ROOT)/mk/package.mk
