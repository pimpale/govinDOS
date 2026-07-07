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
  clang-22 \
    -std=c23 \
    -target x86_64-unknown-windows \
    -ffreestanding  -fno-builtin -fshort-wchar -mno-red-zone \
    -mgeneral-regs-only \
    -fstack-protector-all \
    -Wframe-larger-than=8192 \
    -O0 -g \
    -DPRINTF_SUPPORT_DECIMAL_SPECIFIERS=0 \
    -DPRINTF_SUPPORT_EXPONENTIAL_SPECIFIERS=0 \
    -Isrc \
    -Iarchsrc/x86_64 \
    -Ivendor \
    -Isrc/stdlib \
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

# Flat-binary assembly. Used for the AP trampoline, which must be position-
# correct at a fixed physical address rather than relocatable.
assemble_bin() {
  mkdir -p bin/$(dirname $1)
  nasm \
    -f bin \
    -o bin/$1.bin \
    $1
}

# Builds the embedded userspace test program as a real PE32+ executable
# with the same toolchain as the kernel. -fixed:no keeps the .reloc
# section: every process loads at a fresh address in the single AS, so
# the kernel loader must be able to rebase. -mgeneral-regs-only because
# the kernel doesn't preserve FPU/SSE state across switches yet.
build_userspace() {
  mkdir -p bin/userspace
  clang-22 \
    -std=c23 \
    -target x86_64-unknown-windows \
    -ffreestanding -fno-builtin -mno-red-zone \
    -mgeneral-regs-only \
    -fno-stack-protector \
    -O1 \
    -c -o bin/userspace/hello.o \
    ../userspace/hello.c
  lld-link \
    -flavor link \
    -subsystem:native \
    -entry:_start \
    -nodefaultlib \
    -fixed:no \
    -dynamicbase \
    -debug:none \
    -out:bin/userspace/hello.exe \
    bin/userspace/hello.o
}

# No args, links all objects everything into kernel.efi
link() {
  mkdir -p bin
  files=$(find ./bin/ -name "*.o")
  lld-link \
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
  compile_c src/acpi.c
  compile_c src/debug.c
  compile_c src/stacks.c
  compile_c src/get_mmap.c
  compile_c src/allocator.c
  compile_c src/cpu_state.c
  compile_c src/spinlock.c
  compile_c src/scheduler.c
  compile_c src/thread.c
  compile_c src/syscall.c
  compile_c src/uaccess.c
  compile_c src/umem.c
  compile_c src/reaper.c
  compile_c src/dummydev.c
  compile_c src/ring.c
  compile_c src/session.c
  compile_c src/pe.c
  compile_c src/stdlib/stdio.c
  compile_c src/stdlib/stdlib.c
  compile_c src/stdlib/string.c
  # generic-data-structure instantiations
  compile_c src/instances/list_dtype_thread_ptr.c
  compile_c src/instances/vec_dtype_ublock_ptr.c
  compile_c src/instances/vec_dtype_process_ptr.c
  compile_c src/instances/vec_dtype_session_ptr.c
  # architecture specific
  compile_c archsrc/x86_64/impl_stack_protector.c
  compile_c archsrc/x86_64/serial.c
  compile_c archsrc/x86_64/panic.c
  compile_c archsrc/x86_64/interrupts.c
  compile_c archsrc/x86_64/enumerate_cpus.c
  compile_c archsrc/x86_64/cpu_hwid.c
  compile_c archsrc/x86_64/percpu.c
  compile_c archsrc/x86_64/lapic.c
  compile_c archsrc/x86_64/smp.c
  compile_c archsrc/x86_64/gdt.c
  compile_c archsrc/x86_64/paging.c
  compile_c archsrc/x86_64/cpu_setup.c
  compile_c archsrc/x86_64/thread_arch.c
  compile_c archsrc/x86_64/scheduler_arch.c
  compile_c archsrc/x86_64/irq.c
  assemble  archsrc/x86_64/gdt.asm
  assemble  archsrc/x86_64/interrupts.asm
  assemble  archsrc/x86_64/context_switch.asm
  # AP trampoline: flat binary first, then COFF wrapper that incbin's it.
  assemble_bin archsrc/x86_64/blobs/ap_trampoline.asm
  assemble     archsrc/x86_64/ap_trampoline_blob.asm
  # C userspace test program, compiled to PE and embedded for the PE
  # loader test (no filesystem yet).
  build_userspace
  assemble     archsrc/x86_64/user_pe_blob.asm
  # vendor
  compile_c vendor/buddy_allocator/buddy_allocator.c
  compile_c vendor/printf/printf.c

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
