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
    -Iinc \
    -Iinc_x86_64 \
    -Ivendor \
    -c -o bin/$1.o \
    $1
}

# No args, links all objects everything into kernel.efi
link() {
  mkdir -p bin
  files=$(find ./bin/ -name "*.o")
  lld-link-19 \
    -flavor link \
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
  compile_c src/init.c
  compile_c src/efi_write.c
  compile_c src/serial_write.c
  compile_c src/allocator.c
  compile_c src/c_builtins.c
  compile_c src_x86_64/serial.c
  compile_c src_x86_64/interrupts.c
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
