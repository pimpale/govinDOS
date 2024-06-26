#!/bin/sh

# It is commonly stated that Make is superior to shell scripts for build systems.
# However, in this case, I have elected to use a shell script to build my kernel.
# Make is complicated to implement myself, and a shell script is 90% as good
# The only caveat is that the kernel will have to be rebuilt each time
# It's all in assembly though, so it should be fine (time wise)

#This script will only work from within the kernel dir 

set -e

# One arg, assembles it and places it inside the build dir
compile_c() {
  mkdir -p bin/$(dirname $1)
  clang-19 \
    -std=c23 \
    -target x86_64-unknown-windows \
    -ffreestanding  -fno-builtin -fshort-wchar -mno-red-zone \
    -O0 -g \
    -Isrc \
    -Iarchsrc/x86_64 \
    -Ivendor \
    -c -o bin/$1.o \
    $1
}

assemble() {
  mkdir -p bin/$(dirname $1)
  nasm \
    -g \
    -f win64 \
    -o bin/$1.o \
    $1
}

# No args, links all objects everything into kernel.efi
link() {
  mkdir -p bin
  files=$(find ./bin/ -name "*.o")
  lld-link-19 \
    -flavor link \
    -debug \
    -subsystem:efi_application \
    -entry:efi_main \
    -out:bin/kernel.efi \
    $files
}

# No arguments, cleans the build directory
clean() {
  rm -rf bin
}

# No arguments, makes everything, printing out the path of the finished product
make() {
  # core kernel
  compile_c src/init.c
  compile_c src/serial_write.c
  compile_c src/allocator.c
  compile_c src/debug.c
  compile_c src/c_builtins.c
  # architecture specific
  compile_c archsrc/x86_64/serial.c
  compile_c archsrc/x86_64/setup_interrupts.c
  assemble  archsrc/x86_64/gdt.asm
  assemble  archsrc/x86_64/idt.asm
  # vendor
  compile_c vendor/buddy_allocator/buddy_allocator.c

  link
}

if [ $# -eq 0 ]; then
  make
else
  for cmd in $@; do
    case "$cmd" in
      make|clean|link)
        $cmd
        ;;
      *)
        echo "Error: subcommand not found" >&2
        exit 1
        ;;
    esac
  done
fi
