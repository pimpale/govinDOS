#!/bin/sh

# A make script to build the system

#This script will only work when executed within the root dir of project

set -e

ISO_NAME="govinDOS.iso"

# Passes all args to the specified subproject
subproject() {
  if [ $# -eq 0 ] ; then
    echo "subproject requires subproject directory, followed by arguments to its make"
    exit 1
  fi
  PROJDIR=$1
  THISDIR=`pwd`
  shift
  cd $PROJDIR
  ./make.sh $@
  cd $THISDIR
}

# No args, links all objects everything into kernel.bin
make() {
  mkdir -p bin
  mkdir -p root
  cp -rT root bin/root
  cp -rT efi bin/efi
  subproject kernel make
  mkdir -p bin/root/EFI/BOOT
  cp -rT kernel/bin/kernel.efi bin/root/EFI/BOOT/BOOTX64.efi
}

# No arguments, cleans the build directory
clean() {
  rm -rf bin
}

cleanall() {
  subproject kernel clean
  clean
}

runkernel() {
  make

 qemu-system-x86_64 \
    -no-reboot \
    -drive if=pflash,format=raw,file=./bin/efi/OVMF.fd \
    -drive format=raw,file=fat:rw:bin/root \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -serial stdio \
    -m 1G \
    -net none \
    # -d int
}

if [ $# -eq 0 ]; then
  make
else
  for cmd in $@; do
    case "$cmd" in
      make|clean|cleanall|runkernel)
        $cmd
        ;;
      *)
        echo "Error: subcommand not found" >&2
        exit 1
        ;;
    esac
  done
fi
