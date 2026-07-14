# Source tree and dependency direction

Status: **implemented 2026-07-14.** First-party userspace source is organized by
package. Package installation is the boundary between components.

```text
govindos/
├── Makefile                    # kernel, packaged userspace, EFI staging, QEMU
├── abi/gdosabi/                # shared kernel↔userspace binary contract
├── docs/technical/
├── images/                     # runtime package selections
├── kernel/
├── mk/                         # reusable Make/abuild/apk integration
├── packages/
│   ├── gdos-abi/               # packages the shared ABI headers
│   ├── gdos-syscalls/          # userspace syscall wrapper headers
│   ├── gdos-libc-dev/          # source + APKBUILD + Makefile
│   ├── gdoslib-dev/
│   ├── nvmed/  pcid/  tests/
│   ├── gdos-init/
│   └── base-files/  gdos-base/
├── ports/                      # future external APKBUILDs + patches only
└── userspace/
    ├── Makefile                # sysroot/repository/image orchestration
    ├── build.mk                # PE/COFF toolchain and sysroot compile rules
    └── program.mk              # uniform first-party executable rule
```

## Decisions

- `abi/gdosabi/` is the canonical binary contract because both the kernel and
  userspace consume it. The `gdos-abi` package exports it as `<gdosabi/...>`;
  source ownership does not move into the package.
- `gdos-syscalls` is separate from `gdos-abi`. It owns the header-only
  userspace syscall API exported as `<gdos/...>` and depends on `gdos-abi`.
- A first-party userspace package owns its source, APKBUILD, and component
  Makefile in one directory under `packages/`. Its local `out/` is private.
- Public artifacts cross component boundaries only through `.apk` packages and
  the generated sysroot. No program adds another package's source directory to
  its include or library paths.
- Dependencies point ABI → syscall API → libraries → programs → init → image.
  Programs never link sibling programs; shared code belongs in a library and
  runtime cooperation uses IPC.
- `gdos-init` obtains programs by asking APK to install their packages into a
  temporary root, then embeds only their executable payloads.
- External `ports/` directories contain recipes, patches, and packaging data,
  not vendored upstream source. Abuild fetches and verifies that source.
- `images/development.packages` is the top-level runtime world. Today its APK
  root is merged onto the ESP; it can later move to a filesystem partition
  without changing package ownership.

## Include and link conventions

- Binary contract: `#include <gdosabi/syscall.h>` from `gdos-abi`.
- Syscall wrappers: `#include <gdos/sys.h>` from `gdos-syscalls`.
- Library interfaces use angle brackets, such as `<kring.h>`, `<stdio.h>`, or a
  port's `<crypto.h>`. Quoted includes are for files owned by the same package.
- A component sets `ULIBS` before including `userspace/build.mk`, normally via
  `userspace/program.mk`. `gdoslib` resolves only to
  `out/sysroot/usr/lib/gdoslib.a`; it never adds a source-tree include path.
- Headers and archives become visible only after APK installs the declaring
  development package. Missing dependencies therefore fail at the package
  boundary instead of being hidden by monorepo-relative paths.
