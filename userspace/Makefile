# Userspace orchestrator (docs/technical/source-tree.md). Every
# component directory owns its own build (its Makefile, its out/); this
# file only knows the dependency order and assembles the results:
#
#   out/init.exe   — init, bound for the ESP (\boot\init.exe)
#   out/fs/        — the userspace filesystem tree: fs/ (the static
#                    skeleton: etc/os-release etc.) + every program at
#                    fs/bin/<p>.exe. Staged onto the ESP by the
#                    top-level Makefile for now; moves to its own
#                    partition once a real filesystem exists.
#
# Dependency order (the tree is the dependency graph): lib -> bin ->
# init. Programs never depend on each other; init consumes bin
# artifacts (its initfs manifest is init/Makefile's INITFS_PROGS).

LIB_DIRS  := lib/c lib/sys
PROG_DIRS := $(wildcard bin/*)
PROGS     := $(notdir $(PROG_DIRS))

.PHONY: all clean $(LIB_DIRS) $(PROG_DIRS) init

all: $(PROG_DIRS) init
	rm -rf out
	mkdir -p out/fs/bin
	cp -rT fs out/fs
	$(foreach p,$(PROGS),cp bin/$(p)/out/$(p).exe out/fs/bin/$(p).exe;)
	cp init/out/init.exe out/init.exe

$(LIB_DIRS):
	$(MAKE) -C $@

$(PROG_DIRS): $(LIB_DIRS)
	$(MAKE) -C $@

init: $(PROG_DIRS)
	$(MAKE) -C init

clean:
	rm -rf out
	for d in $(LIB_DIRS) $(PROG_DIRS) init; do $(MAKE) -C $$d clean; done
