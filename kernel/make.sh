#!/bin/sh

# It is commonly stated that Make is superior to shell scripts for build systems.
# However, in this case, I have elected to use a shell script to build my kernel.
# Make is complicated to implement myself, and a shell script is 90% as good
# The only caveat is that the kernel will have to be rebuilt each time
# It's all in assembly though, so it should be fine (time wise)

#This script will only work from within the kernel dir 

# One arg, assembles it and places it inside the build dir
assemble() {
  nasm -f elf64 $SRC_DIR/$1 -o $BUILD_DIR/obj/$1.o
}

# No args, links all objects everything into kernel.bin
link_all() {
  gcc -mcmodel=large -ffreestanding -nostdlib -T linker.ld \
    -o $BUILD_DIR/obj/kernel.bin $BUILD_DIR/*.o
}

# No arguments, cleans the build directory
clean() {
  rm -rf $BUILD_DIR
}

# No arguments, makes folders and things
src() {
  clean
  mkdir -p $BUILD_DIR
  mkdir $BUILD_DIR/obj # store objects binaries
  assemble header32.asm
  assemble start32.asm
  link_all
}



