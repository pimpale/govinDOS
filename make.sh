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
  cp -r root bin/root
  cp -r efi bin/efi
  subproject kernel make
  cp kernel/bin/kernel.efi bin/root/
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
    -drive if=pflash,format=raw,file=./bin/efi/OVMF.fd \
    -drive format=raw,file=fat:rw:bin/root \
    -monitor stdio \
    -net none
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
