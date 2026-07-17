# govinDOS

GovinDOS is built as a native APK package graph. First-party `APKBUILD` recipes
produce a signed local repository, development packages form the target
sysroot, and an image profile selects the runtime filesystem with `apk add`.

Install clang-22, lld-link, llvm-ar, nasm, cpio, GNU make, QEMU, Bear,
`apk-tools`, `abuild`, `buildrepo`, `fakeroot`, GNU tar, LuaFileSystem, and Lua
optarg. Configure an abuild signing key with `abuild-keygen -a`, then run:

```sh
make runkernel
```

`make` builds the kernel, native package repository, development sysroot, and
runtime image. Use `make -C packages` or `make -C ports` to publish only one APK
repository. The repository-local `.abuild` selects the GovinDOS cross toolchain
automatically. `make compdb` uses Bear to regenerate `compile_commands.json`.

The build and package layout is documented in
[`docs/technical/package-build.md`](docs/technical/package-build.md).
