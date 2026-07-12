# Source tree: monorepo layout, the gdos ABI, dependency direction

Status: **implemented 2026-07-08.** Everything needed for a functional
system lives in this repo. The tree is the dependency graph: directories
delineate build order, and anything "odd" is odd by placement, not by
convention buried in a Makefile.

```
govindos/
├── Makefile                 # orchestration: subprojects, ESP staging, runkernel
├── docs/technical/
├── efi/                     # OVMF — dev harness, not part of the OS
├── root/                    # static seed of the ESP tree
├── abi/
│   └── gdos/                # the kernel↔userspace contract (see below)
├── kernel/
│   ├── src/  archsrc/x86_64/  vendor/
│   │   └── src/schemes/     # one file per kernel-channel scheme (shares, tree, groups)
└── userspace/
    ├── Makefile             # orchestrator: dependency order + assembly, no compiling
    ├── build.mk             # shared toolchain/flags/rules for all components
    ├── fs/                  # static skeleton of the userspace filesystem (etc/os-release, ...)
    ├── lib/
    │   ├── sys/             # the OS interface: syscall wrappers, kring, PE loader, cpio
    │   ├── c/               # minimal libc — no POSIX, for first-party code
    │   └── posix/           # (future) POSIX layer for porting; a client of sys + c
    ├── bin/                 # one self-contained directory per program
    │   └── tests/           # the ring-3 suite; later: fs/, ls/, cat/, ...
    └── init/                # odd on purpose (see below)
```

Every component directory (each lib, each program, init) has its own
Makefile and builds into its own gitignored `out/` — as components grow
(and they will), a single centralized Makefile stops being tractable, so
the unit of build ownership is the directory. `userspace/build.mk`
holds the shared toolchain, flags, and pattern rules; a component
Makefile sets `U` (its path back to `userspace/`), includes build.mk,
and states only *what* it builds — typically five lines. The kernel and
the top level use `out/` the same way; `bin/` is a *source* directory.

## 0. Decisions log

- **`abi/gdos/` is the single home of the kernel↔userspace contract**:
  syscall numbers, `SYSERR_*`, `VM_PROT_*`, `REAP_*`
  (`<gdos/syscall.h>`), the kring header/SQE/CQE layout, scheme ids and
  `KEV_*` vocabularies (`<gdos/kring*.h>`), and the bootinfo struct
  (`<gdos/bootinfo.h>`). Both builds add `-I../abi`; the kernel's
  `syscall.h`/`channel.h` include these rather than defining their own.
  This killed a real defect class: userspace previously *hand-mirrored*
  the constants ("mirror of kernel/src/channel.h") and included
  bootinfo.h by reaching into `kernel/src/`. "gdos" is the project
  shorthand; the `abi/` nesting keeps the include root scoped so
  userspace can't casually `#include <kernel/...>`.
- **`bin/<program>/`: one self-contained directory per program, no
  taxonomy below that.** No servers-vs-commands split (they are the same
  kind of thing with the same build pattern; the lifecycle difference is
  a runtime/docs matter), no single-file special cases, and the test
  suite is just a normal program init happens to run. Each program's
  Makefile builds every `.c` in its directory into `out/<p>.exe`;
  adding a program is `mkdir` + write code + a five-line Makefile.
- **Dependency direction is a rule, not a suggestion**: `abi/` depends
  on nothing; `lib/` depends on `abi/`; `bin/*` depends on `lib/` +
  `abi/` and **never on a sibling** — programs talk via IPC at runtime,
  not via each other's code at build time. If two programs need shared
  code, that code is telling you it's a `lib/`. `init/` depends on
  `lib/`, `abi/`, and the built *artifacts* of `bin/*`, never their
  sources. Build order falls out: abi → lib → bin → init → staging.
- **`init/` is deliberately outside `bin/`** because it behaves
  differently in exactly the ways the tree should flag: it packages a
  subset of the other programs' *artifacts* into itself at build time
  (the initfs), and the top-level build puts it on the ESP rather than
  the filesystem. Which subset is the `INITFS_PROGS` manifest in
  `init/Makefile` — the packaging decision lives with the packager.
  Unlisted programs still build and ship on the filesystem tree.
- **Userspace's assembled output is `out/init.exe` + `out/fs/`** — the
  filesystem tree: the static `fs/` skeleton (`etc/os-release`, ...)
  plus every program at `fs/bin/<p>.exe`. The orchestrator
  (`userspace/Makefile`) compiles nothing itself; it builds components
  in dependency order and assembles. The top-level Makefile stages
  kernel + init.exe onto the ESP and, **for now, merges `out/fs/` onto
  the ESP too** — `EFI/` and `boot/` don't collide with the unix dirs —
  until a real filesystem gives the tree its own partition.
- **`lib/` stays subdivided (`sys` / `c` / future `posix`)** even though
  it is all "lib": `c` must never know the OS interface exists (it is
  the generic C runtime), and `posix` will be a *client* of `sys`
  (implementing `open`/`read` as channel protocol against the fs
  server) — boundaries that are cheap to keep and expensive to
  re-separate. Fuchsia's libzircon/fdio/musl split is the precedent.
- **Third-party stays quarantined**: `kernel/vendor/` today; ported
  software later goes in a top-level `ports/` (recipes + patches built
  against `lib/posix`), never mixed into `userspace/`. The name is
  reserved; the directory gets created when the first port exists.
- **Libraries are `llvm-ar` archives** (`lib/sys/out/sys.a`,
  `lib/c/out/c.a`); lld-link pulls only the members a program
  references. build.mk gives every component a build-if-missing rule
  for the archives, so `make -C userspace/bin/<p>` works from a clean
  tree; cross-component *staleness* is the orchestrator's job — run the
  top-level make when libs change.

## 1. What deliberately isn't here

No deep nesting (one developer, ~10 components — two levels is plenty).
No build-system change (recursive make with pattern rules scales to this
shape). No per-component test directories (one suite binary is the
right size). No `userspace/init` merged into `bin/` for uniformity's
sake — uniformity that hides real difference is worse than none.

## 2. Include conventions

- Shared contract: `#include <gdos/syscall.h>` (angle brackets, via
  `-I../abi`).
- Within userspace: a component's Makefile picks its libraries by
  setting `ULIBS` (names under `lib/`) before including build.mk, which
  turns that into `-I$(U)/lib/<l>` include dirs and the archive list
  `LIBS` for the link. Library headers are included with angle brackets
  — `#include <sys.h>`, `<kring.h>`, `<pe.h>`, `<cpio.h>`, `<string.h>`
  — the same way as the ABI's `<gdos/...>`: everything a component
  consumes without owning. Quoted includes are for a directory's own
  headers only. `lib/c`'s headers use the standard names (`string.h`,
  `strlen`, `memcpy`) — freestanding builds get no compiler-provided
  libc headers, so there is no collision, and code reads like normal C.
  (lib/sys names carry no `u` prefix: everything in userspace is
  userland, so `usys.h`/`upe.c`/`ukring` would say nothing — the prefix
  only earns its keep in the kernel tree, where `pe.c` and `syscall.h`
  already mean the kernel's own.)
- Kernel: unchanged (`-Isrc -Iarchsrc/x86_64 -Ivendor -Isrc/stdlib`)
  plus `-I../abi`.
