#!/bin/sh

# It is commonly stated that Make is superior to shell scripts for build systems.
# However, in this case, I have elected to use a shell script to build my kernel.
# Make is complicated to implement myself, and a shell script is 90% as good
# The only caveat is that the kernel will have to be rebuilt each time
# It's all in assembly though, so it should be fine (time wise)

#This script will only work from within the kernel dir 

set -e

INSTALL_DIR="../bin/sysroot/boot"

# One arg, assembles it and places it inside the build dir
assemble() {
  mkdir -p bin
  nasm -i inc -f elf64 src/$1 -o bin/$1.o
}

# No args, links all objects everything into kernel.bin
link() {
  gcc -mcmodel=large -ffreestanding -nostdlib -T linker.ld \
    -o bin/kernel.bin bin/*.o
}

# No arguments, cleans the build directory
clean() {
  rm -rf bin
}

# No args. installs in install dir
install() {
  mkdir -p $INSTALL_DIR
  cp bin/kernel.bin $INSTALL_DIR
}

# No arguments, makes everything, printing out the path of the finished product
make() {
  assemble debug.asm
  assemble early_init.asm
  assemble header.asm
  assemble init.asm
  assemble vga.asm
  link
}

if [ $# -eq 0 ]; then
  make
else
  for cmd in $@; do
    case "$cmd" in
      make|clean|link|install)
        $cmd
        ;;
      *)
        echo "Error: subcommand not found" >&2
        exit 1
        ;;
    esac
  done
fi
