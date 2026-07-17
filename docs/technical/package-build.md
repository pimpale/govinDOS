# Native APK system build

Status: **implemented 2026-07-16.** GovinDOS uses Alpine's native source and
binary package tooling at build time:

- `APKBUILD` defines package metadata, build dependencies, and runtime policy;
- `buildrepo` reads the aports-shaped source tree and schedules recipes;
- `abuild -r` installs declared dependencies and produces signed APKs;
- `apk add --root` constructs target build roots, the SDK, and runtime images.

There is no second package graph in GNU Make.

## Build pipeline

```text
packages/*/APKBUILD
        │ buildrepo orders depends + makedepends_host
        ▼
abuild -r
        │ apk installs the recipe's target dependency closure
        ├────────► out/buildroot
        │ make compiles and DESTDIR-installs
        ▼
out/repository/packages/x86_64-govindos/
        ├── <name>-<version>.apk
        └── APKINDEX.tar.gz
                     │ apk add --root
                     ├────────► out/sysroot     (gdoslib-dev closure)
                     └────────► out/image-root  (image profile)
```

`out/buildroot` is transient. Before each recipe, abuild installs that recipe's
`makedepends_host` closure; after packaging it removes the closure. This catches
undeclared dependencies instead of allowing later packages to inherit an
ever-growing sysroot.

`out/sysroot` is a product, not build state. The default top-level build installs
the `gdoslib-dev` closure there, selecting all target headers and archives used
by clangd and direct component builds. `out/image-root` contains the runtime
closure selected by `images/development.packages`. The top-level build adds the
kernel and stages the image as `out/root` for QEMU.

## Dependency fields

GovinDOS cross builds use APK's dependency classes as follows:

| APKBUILD field | Meaning |
| --- | --- |
| `depends` | Packages required when this package is installed |
| `makedepends_host` | GovinDOS target headers, archives, or programs required while building |
| `makedepends_build` | Tools executed by the Linux build environment |

For example, `nvmed` has `makedepends_host="gdoslib-dev"` but no runtime
library dependency because it is statically linked. `gdos-init` lists `nvmed`,
`pcid`, and `tests` as both runtime dependencies and target build inputs because
it embeds their APK-installed executables. `gdos-base` is an empty metapackage
whose dependency closure defines the runtime world.

In cross mode, abuild also analyzes a recipe's `depends` while constructing its
build environment. Target-only dependencies of `noarch` packages are therefore
mirrored in `makedepends_host`; this keeps them in `out/buildroot` instead of
asking the disposable x86_64 Linux root to install GovindOS packages.

The first-party graph is therefore declared only in APKBUILDs:

```text
gdos-abi -> gdos-syscalls -> gdos-libc-dev -> gdoslib-dev
                                                    ├──> nvmed ──┐
                                                    ├──> pcid ───┼──> gdos-init
                                                    └──> tests ──┘         │
base-files ────────────────────────────────────────────────────────> gdos-base
```

## Package contract

A normal first-party program has this shape:

```text
packages/cryptod/
├── APKBUILD
├── Makefile
└── cryptod.c
```

Its Makefile describes compilation and installation only:

```make
PROGRAM := cryptod
SOURCES := cryptod.c
OBJECTS := out/cryptod.o
LDLIBS := -lcrypto -lgdoslib -lc

all: out/cryptod.exe

out/cryptod.o: cryptod.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

out/cryptod.exe: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

install: out/cryptod.exe
	install -Dm755 $< $(DESTDIR)/bin/cryptod.exe
```

The names follow the selected COFF toolchain's linker conventions: development
packages install `foo.lib`, and consumers add `-lfoo` to `LDLIBS`. Component
Makefiles inherit `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `AR`, and the target
sysroot from abuild.

Its APKBUILD owns build and runtime dependency policy:

```sh
pkgname=cryptod
pkgver=0.1.0
pkgrel=0
pkgdesc="GovinDOS cryptography service"
url="https://github.com/pimpale/govindos"
arch="x86_64-govindos"
license="LicenseRef-GovindOS"
makedepends_host="libcrypto-dev gdoslib-dev"
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

First-party recipes deliberately build live in-tree source from `$startdir`.
Buildrepo normally treats an existing versioned APK as current, so
`packages/Makefile` places a repository stamp over all checked-in package inputs
and the shared ABI. Any such change rebuilds the small native repository as a
unit, preserving useful monorepo iteration without inventing package metadata.
External ports use conventional immutable `source=` distfiles and checksums.

## Host and target roots on Arch

Abuild normally assumes the build host itself has an APK database. The Arch
host does not, so `tools/apk-root.sh` redirects unrooted build-package
operations to disposable `out/host-buildroot`. Operations carrying abuild's
`--root` continue to use `out/buildroot`, whose architecture is
`x86_64-govindos`. Neither operation mutates the Arch package database.

The repository-root `.abuild` is the version-controlled cross-toolchain
configuration. Abuild sources it because buildrepo supplies `APORTSDIR`. It
puts the `x86_64-pc-windows-msvc-*` wrappers under `toolchain/bin/` on `PATH`,
and supplies target flags, PE linker policy, and `out/buildroot` as `SYSROOT`
in the build environment. Abuild selects those tools through its normal
`${CHOST}-` cross prefix. The compiler wrapper uses Clang's supported
`x86_64-pc-windows-msvc` backend to emit PE/COFF; `x86_64-govindos` remains the
APK package architecture.

`abuild.env` supplies the Arch-host APK adapter and then loads `.abuild`. It uses
the `export NAME=value` and `.` forms shared by bash, zsh, fish, and POSIX
shells. `apk.mk` loads this same file in the recipe shell before invoking
buildrepo, so interactive and Make-driven builds have one environment definition.

Package Makefiles use standard build variables rather than knowing the Clang
target or linker command line. Each Makefile contains its own ordinary object,
link, dependency-file, clean, and `DESTDIR` installation rules.

The current first-party recipes use preinstalled host tools rather than
`makedepends_build`. When ports begin declaring Alpine-managed host tools, run
the repository builder in an Alpine build container or configure an appropriate
host APK repository.

## External ports

External software belongs under `ports/<name>/`. A port contains only its
APKBUILD, patches, and packaging files. Upstream source is declared in
`source=`, verified by abuild checksums, and unpacked into generated `$srcdir`.
`make -C ports` publishes these packages to `out/repository/ports/`.

The sysroot and image installers automatically enable the ports repository when its
signed index exists. Cross-repository source scheduling will need an explicit
repository order once the first native package consumes a port; buildrepo
currently schedules one source repository at a time.

## Host setup and targets

Install clang-22, lld-link, llvm-ar, nasm, cpio, GNU make, QEMU, Bear,
`apk-tools`, `abuild`, `buildrepo`, `fakeroot`, GNU tar, LuaFileSystem, and Lua
optarg. On Arch, `buildrepo`'s Lua dependencies can be installed with:

```sh
sudo pacman -S lua-filesystem luarocks
sudo luarocks --lua-version=5.5 install optarg
```

Configure an abuild signing key with `abuild-keygen -a`. The Arch abuild package
runs its fakeroot pass through BusyBox; the root `apk.mk` selects `fakeroot bash`
so GNU tar is used.

Useful targets:

```sh
make                         # kernel + repository + sysroot + runtime image
make -C packages             # build and index all first-party APKs
make -C ports                # build the external repository, when nonempty
make compdb                  # regenerate compile_commands.json
make cleanall
```

For a direct dependency-aware recipe build, load the same repository environment
used by Make and invoke abuild. The `.` spelling works in bash, zsh, and fish:

```sh
. ./abuild.env
abuild -C packages/tests -P "$PWD/out/repository" -f -r
```
