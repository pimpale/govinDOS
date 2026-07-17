# Source tree and dependency direction

Status: **implemented 2026-07-16.** First-party userspace source is organized by
package. Package installation is the boundary between components, and APKBUILD
metadata is the source of truth for build and runtime dependency order.

```text
govindos/
├── .abuild                     # repository-local cross-toolchain policy
├── apk.mk                      # APK repositories and root installation
├── Makefile                    # kernel, packaged userspace, EFI staging, QEMU
├── abi/gdosabi/                # shared kernel/userspace binary contract
├── docs/technical/
├── images/                     # runtime package selections
├── kernel/
├── packages/
│   ├── Makefile                # native repository publication
│   ├── gdos-abi/               # packages the shared ABI headers
│   ├── gdos-syscalls/          # userspace syscall wrapper headers
│   ├── gdos-libc-dev/          # source + APKBUILD + Makefile
│   ├── gdoslib-dev/
│   ├── nvmed/  pcid/  tests/
│   ├── gdos-init/
│   └── base-files/  gdos-base/
├── toolchain/bin/              # CHOST-prefixed LLVM wrappers
├── tools/apk-root.sh           # Arch-host APK database adapter
└── ports/
    ├── Makefile                # external repository publication
    └── <port>/                 # APKBUILD + patches/packaging data
```

## Decisions

- `abi/gdosabi/` is the canonical binary contract because both the kernel and
  userspace consume it. `gdos-abi` exports it as `<gdosabi/...>`; source
  ownership does not move into the package.
- `gdos-syscalls` owns the header-only userspace syscall API exported as
  `<gdos/...>` and depends on `gdos-abi`.
- A first-party userspace package owns its source, APKBUILD, and component
  Makefile in one directory under `packages/`. Its local `out/` is private.
- Package Makefiles contain their complete compile, link, clean, and `DESTDIR`
  installation rules. They include no repository Make fragments and do not
  invoke their own APKBUILD. `buildrepo` invokes `abuild -r` for them.
- The root `.abuild` supplies standard compiler, assembler, archiver, sysroot,
  and linker variables. Prefixed wrappers select Clang's PE/COFF backend, so
  package Makefiles contain no GovinDOS target flags.
- `depends` declares installed-package relationships. `makedepends_host`
  declares GovinDOS target packages needed while building. Buildrepo derives
  ordering from those fields; there is no parallel Make-only package graph.
- Public artifacts cross component boundaries only through `.apk` packages and
  an APK-installed target root. No program adds another package's source tree
  to its include or library paths.
- Dependencies point ABI -> syscall API -> libraries -> programs -> init ->
  image. Programs never link sibling programs; shared code belongs in a library
  and runtime cooperation uses IPC.
- `gdos-init` declares the program packages as build dependencies. Abuild
  installs them into its target build root, and init embeds those installed
  executable payloads.
- A checked-in package may keep implementation in `source/`; it must not use
  abuild's reserved `src/` extraction workspace.
- External ports contain recipes, patches, and packaging data, not vendored
  upstream source. Abuild fetches and verifies their declared distfiles.
- `images/development.packages` is the runtime world. The default build installs
  the `gdoslib-dev` dependency closure into `out/sysroot` as the development
  world for compilers and clangd.

## Include and link conventions

- Binary contract: `#include <gdosabi/syscall.h>` from `gdos-abi`.
- Syscall wrappers: `#include <gdos/sys.h>` from `gdos-syscalls`.
- Library interfaces use angle brackets, such as `<kring.h>`, `<stdio.h>`, or a
  port's `<crypto.h>`. Quoted includes are for files owned by the same package.
- Static libraries use conventional COFF `foo.lib` names. A component adds
  `-lfoo` to standard Make `LDLIBS`. The linker searches only the APK-installed
  target root; it never adds a source-tree library path.
- Missing or undeclared build dependencies fail at the package boundary because
  abuild removes each package's temporary dependency closure after the build.
