# govinDOS

GovinDOS is built as a native APK package graph. First-party `APKBUILD` recipes
produce a signed local repository, development packages form the target
sysroot, and an image profile selects the runtime filesystem with `apk add`.

Install clang-22, lld-link, llvm-ar, nasm, cpio, GNU make, QEMU, Bear,
`apk-tools`, `abuild`, `fakeroot`, and GNU tar. Configure an abuild signing key
with `abuild-keygen -a`, then run:

```sh
make runkernel
```

Useful build-only targets are `make`, `make -C userspace sysroot`, and
`make -C userspace image`. `make compdb` uses Bear to regenerate
`compile_commands.json` for clangd.

The build and package layout is documented in
[`docs/technical/package-build.md`](docs/technical/package-build.md).
