# Native APK system build

Status: **implemented 2026-07-14.** GovinDOS uses Alpine's native package
toolchain at build time:

- `APKBUILD` is the package formula;
- `abuild` is the formula runner and package builder (the APK equivalent of
  Arch's `makepkg`);
- `abuild index` creates and signs the repository index;
- `apk add --root ...` resolves dependencies and constructs sysroots and images.

There is no local package metadata generator or filesystem merge script.

## Build pipeline

```text
packages/<name>/{APKBUILD, source, Makefile}
       │ make compiles against out/sysroot
       │ abuild package() installs into $pkgdir
       ▼
out/repository/packages/x86_64-govindos/
       ├── <name>-<version>.apk
       └── APKINDEX.tar.gz
                    │ apk add --root
                    ├────────► out/sysroot
                    └────────► out/image-root
```

`out/sysroot` contains the selected development-package closure. Clang, lld,
and clangd consume headers and archives only from this root. `out/image-root`
contains the runtime closure selected by `images/development.packages`, along
with APK's installed database. The top-level build adds the kernel and stages
that result as `out/root` for QEMU.

APK therefore owns dependency solving, signature verification, package install
order, file ownership conflicts, and the installed package database.

## `mk/` versus `packages/`

These directories operate at different levels:

| Directory | What belongs there | Example |
| --- | --- | --- |
| `packages/` | Package-specific source, `APKBUILD`, patches/data, and its small source Makefile | `packages/nvmed/APKBUILD` |
| `mk/` | Reusable GNU Make implementation shared by every package | `mk/package.mk` |

`packages/` answers “what is this package and how is it built?” Its `APKBUILD`
owns `pkgname`, `pkgver`, dependencies, options, and the `build()`/`package()`
phases. First-party userspace source is colocated with that formula.

`mk/` answers “how does the monorepo invoke the package toolchain?”
`mk/package.mk` maps a component artifact to `abuild rootpkg`.
`mk/apk.mk` centralizes repository paths and the `apk add --root` invocation.
`mk/version.mk` defines the custom package architecture. None of these files is
a package recipe and none contains package dependencies.

## First-party package graph

```text
gdos-abi ──► gdos-syscalls ──► gdos-libc-dev ──► gdoslib-dev
                                                       ├──► nvmed ──┐
                                                       ├──► pcid ───┼──► gdos-init ──┐
                                                       └──► tests ──┘                ├──► gdos-base
base-files ──────────────────────────────────────────────────────────────────────────┘
```

The split at the beginning is intentional:

- `gdos-abi` packages the passive kernel/userspace contract from
  `abi/gdosabi/` as `<gdosabi/...>`;
- `gdos-syscalls` packages the userspace syscall wrapper API as `<gdos/...>` and
  depends on `gdos-abi`.

The development packages build the target sysroot. Programs are statically
linked and therefore have no runtime dependency on those development packages.
`gdos-init` depends on the program packages and builds its embedded initfs from
an APK-installed root. `gdos-base` is a payload-free metapackage whose dependency
closure defines the default image.

## First-party package contract

A source package normally has this shape:

```text
packages/cryptod/
├── APKBUILD
├── Makefile
└── cryptod.c
```

Its Makefile describes compilation and a `DESTDIR`-aware install:

```make
U := ../../userspace
PROGRAM := cryptod
ULIBS := crypto gdoslib c
include $(U)/program.mk
```

Its APKBUILD contains package policy:

```sh
pkgname=cryptod
pkgver=0.1.0
pkgrel=0
pkgdesc="GovinDOS cryptography service"
url="https://github.com/pimpale/govindos"
arch="x86_64-govindos"
license="LicenseRef-GovindOS"
options="!check !strip !tracedeps !archcheck !usrmerge"
source=""
builddir="$startdir"

build() {
	make -C "$builddir"
}

package() {
	make -C "$builddir" install DESTDIR="$pkgdir"
}
```

The orchestrator invokes first-party recipes with `abuild -f`: Make already
tracks their live in-tree source dependencies, while forcing abuild avoids
pretending those files are immutable downloaded distfiles.

## External ports

External software belongs under `ports/<name>/`. A port contains only its
`APKBUILD`, patches, and other packaging files. Upstream source is named by the
APKBUILD's `source=` field, verified by abuild checksums, and unpacked into
abuild's generated `$srcdir`; it is not checked into the package directory.

Port-specific compiler/configure flags and patches stay in that recipe. A
ported library should produce a development package containing, for example,
`/usr/include/crypto.h` and `/usr/lib/crypto.a`. After APK installs that package
into `out/sysroot`, an in-tree `cryptod` names `crypto` in `ULIBS` and includes
`<crypto.h>` normally. This gives ports and first-party code the same compiler
environment.

When a ports repository is added, it should get its own signed repository under
`out/repository/ports/`; sysroot and image installation can pass both native APK
repositories to the solver.

## clangd

`make compdb` runs the real clean build under Bear. Its userspace commands
contain both:

```text
-target x86_64-unknown-windows
-I/home/.../govindos/out/sysroot/usr/include
```

Consequently clangd resolves headers installed by first-party packages and
ports exactly as clang does. Refresh the database after adding a source file or
changing compiler flags.

## Host setup and targets

In addition to the compiler and emulator dependencies, the build requires
`apk-tools`, `abuild`, `fakeroot`, GNU tar, and an abuild signing key. Configure
the key with `abuild-keygen -a`; its public half is read from `$HOME/.abuild`
when APK verifies the local repository.

The Arch Linux `abuild` package runs under a standalone busybox shell whose
internal `tar` lacks options required by abuild. `mk/apk.mk` routes the fakeroot
pass through Bash so abuild selects GNU `/usr/bin/tar`; the APKBUILDs themselves
remain standard.

Useful targets:

```sh
make                         # kernel + package-selected system image
make -C userspace sysroot    # rebuild the development package closure
make -C userspace packages   # build and index the default package set
make -C userspace image      # apk-install the selected runtime closure
make -C packages/nvmed       # direct first-party component build
make compdb                  # regenerate compile_commands.json
make cleanall
```
