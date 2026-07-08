# Boot & init design: the ESP contract, initfs, bootinfo

Status: **implemented 2026-07-08** (same day as designed) — §5 steps 1–3
all landed: init off the ESP, bootinfo + entry ABI, initfs + test-suite
split + userland PE loader. **§4 (design C, the bootfs server) remains
future work**, gated on the name-registry scheme. §6 records where the
implementation deliberately diverges. Companion to
[ipc-process-design.md](ipc-process-design.md) — this document answers
"how do blobs get into init's hands"; that one's §5 (parent-driven
creation) already answers "how children get built from those blobs" and
is taken as given here. Supersedes the current embedded-init mechanism
(`user_pe_blob.asm` + `build_userspace` inside the kernel build), which
it deletes.

Current state this replaces: `userspace/hello.c` is compiled to PE32+
*inside the kernel build*, incbin'd into the kernel image as
`user_pe_blob`, and loaded by the kernel's boot-only PE loader
(`kernel/src/pe.c`) as the root of the process tree.

## 0. Decisions log

- **The kernel loads exactly one userspace artifact: `\boot\init.exe`
  from the ESP.** The kernel stub reads it via UEFI boot services
  (SimpleFileSystem on the kernel's own loaded-image device) before
  `ExitBootServices`, PE-loads it with pe.c, and jumps. That is the
  kernel's entire userspace contract — the seL4 shape (one root task),
  chosen for the same reason: everything else is init's problem.
  UEFI is our free bootloader-module mechanism; the ESP FAT is already
  the qemu `fat:rw:bin/root` staging dir, so "add a boot file" is a
  Makefile `cp`.
  - *Rejected: init linked into the kernel image* (status quo). Keeps
    the kernel-build-depends-on-userspace-build wart, relinks the
    kernel on every init change, and still needs ESP-read code for the
    archive — both costs, neither benefit. Its apparent robustness edge
    is illusory: `kernel.efi` comes off the same FAT; if boot-services
    file reads fail we never booted at all.
  - *Rejected: separate `initdata.cpio` on the ESP.* Requires a new
    kernel concept — an anonymous preloaded data block placed in the
    AS, minted as a ublock, billed to init, described in bootinfo —
    plus a version-skew surface between init.exe and its archive.
- **The initfs rides inside init.exe as a linked section.** The archive
  (`initdata.cpio`) is incbin'd into a tiny COFF object exporting
  `bootfs_start`/`bootfs_end` — the same trick as today's
  `user_pe_blob.asm`, moved into the userspace link. pe.c maps it like
  any other read-only section; the kernel never learns the archive
  exists. Init references two extern symbols and has the bounds — no
  self-PE-parsing, no bootinfo entry, no skew (init and its data are
  one atomic artifact; the only remaining skew surface is the
  kernel↔init syscall ABI, which exists regardless). Not a one-way
  door: if initdata grows large (driver firmware), splitting it back
  out is small precisely because init already parses an archive — only
  discovery moves (bootinfo pointer instead of linked symbols).
- **Archive format: cpio newc.** Generated with
  `find | cpio -o -H newc`, parsed in ~50 lines of freestanding C.
  Known limitation, accepted: members are 4-byte aligned, so zero-copy
  `VM_SHARE` of individual members is off the table (page-aligned
  custom format — the QNX mkifs move — if a concrete zero-copy data
  use case ever appears). For executables alignment is moot anyway:
  spawning into fresh SASOS addresses is relocate-and-copy regardless
  (ipc doc §5).
- **A bootinfo block is the entry ABI, and it exists for more than the
  initfs.** Drivers are userspace, so everything only discoverable
  before `ExitBootServices` — ACPI RSDP, GOP framebuffer, memory
  stats — must be captured by the stub and handed to init. One
  read-only ublock, owned by init, base passed in a register at first
  entry. The initfs needs *no* bootinfo entry (linked symbols, above);
  bootinfo is the hardware-handoff channel.
- **Consume now, serve later.** Near term init parses the archive
  itself and spawns children per ipc doc §5. The endpoint (§4 below)
  is the QNX/L4Re shape: a bootfs server exposing the archive as a
  read-only `boot:` namespace over ordinary channels, registered in
  the future name-registry scheme, so every process opens its binary
  through the same open/read/spawn path the real fs server will use
  and the disk-root pivot is just a registry rebind. Gated on the name
  registry and pe.c-as-userland-library; explicitly not phase one.
- **Init's job narrows: spawn servers, hold the tree root, reap.** The
  ring-3 test suite currently fused into hello.c splits out into a
  child binary shipped in the initfs — which makes it the first real
  consumer of the whole mechanism.

## 1. Precedents

- **seL4**: kernel loads one root task; everything else is a CPIO
  linked into the root task's own ELF. The shape adopted here.
- **Zircon/Fuchsia**: ZBI carries kernel + bootfs; `userboot` parses it
  and starts component manager; bootfs stays *served* read-only until
  the real fs is up. Source of the consume→serve evolution.
- **QNX**: the boot image *is* a filesystem (mkifs IFS), mounted
  read-only, binaries executed straight out of it (members page-padded
  for XIP — the alignment lesson noted in §0).
- **L4Re**: multiboot modules exposed by the root task as a read-only
  `rom:` namespace; init (`ned`) spawns from that namespace.
- **Redox**: kernel loads a bootstrap blob containing initfs; init
  starts drivers from it, then pivots the scheme namespace to disk —
  the closest cousin to our scheme model.
- Convergent lessons: the kernel loads exactly one thing, and the
  archive is served as a namespace, not unpacked-and-discarded.

## 2. The kernel's userspace contract

Everything the kernel does for userspace at boot, exhaustively:

1. Before `ExitBootServices`: read `\boot\init.exe` from the boot
   volume into memory; capture RSDP, framebuffer info, and anything
   else bootinfo carries (§3).
2. After the existing memory/paging bring-up: PE-load init.exe (pe.c,
   unchanged role — boot-only loader, relocations against its fresh
   SASOS base, per-section W^X views).
3. Build the bootinfo ublock, map it read-only into init.
4. Spawn init's first thread with the bootinfo base in a register
   (exact register: pick alongside the entry-ABI implementation; it is
   the *only* entry argument).

pe.c stays kernel-resident but is used exactly once per boot. Children
are built by their parents (ipc doc §5); the loader logic migrates to a
userland library when the test-suite child lands (§5 below).

Failure is fatal and loud: missing `\boot\init.exe`, unparseable PE, or
an import table (still rejected — freestanding only) panic the boot
with the reason on serial. There is no fallback init.

## 3. The bootinfo block

One page (grow if needed), read-only view in init, owned by init and
billed to it like any ublock. Contents, initial set:

- magic + version + total length
- ACPI RSDP physical address
- framebuffer: base, pitch, width, height, pixel format (GOP capture)
- memory: total/usable page counts at handoff (informational; authority
  over memory stays with the kernel's allocator + quota model)

Deliberately *not* in bootinfo: the initfs (linked symbols instead,
§0), a command line (no use case yet; add a field when one exists).
Layout is a versioned C struct in a shared header
(`userspace`↔`kernel`), not TLV — one producer, one consumer, same
repo, same commit.

## 4. The bootfs endpoint (future: design C)

When the name-registry scheme exists: init spawns a bootfs server as
its first child, hands it the archive, and the server registers
`boot:`. It answers open/read (and later readdir) over ordinary
channels; exec becomes a userland library that reads an image from any
namespace and runs the §5 creation protocol. Early boot stops being
special: the same path later serves the real root, and the pivot is a
registry rebind, after which bootfs can retire or stay mounted as
`boot:` for diagnostics.

**Flagged open question — share granularity.** The archive lives
inside init's *image* mapping, adjacent to init's writable `.data`. If
`VM_SHARE` turns out to be whole-ublock-granular, "hand the server just
the archive" means either sharing init's whole image (leaks init's
mutable state to the server — no) or copying the archive into a fresh
ublock at handoff (one memcpy, accepted if needed). Decide when design
C lands; if sub-block sharing exists by then, share the section
directly.

## 5. Implementation order

1. **Move init to the ESP.** Stub reads `\boot\init.exe`; delete
   `user_pe_blob.asm` and `build_userspace` from the kernel build.
   Userspace becomes a sibling build (own Makefile under `userspace/`)
   staged into `bin/root/boot/` by the top-level Makefile. Iterating on
   init no longer relinks the kernel.
2. **Bootinfo block + entry ABI** (§3). RSDP capture unblocks future
   userspace ACPI work; do it in the same pass as the file read since
   both are pre-`ExitBootServices`.
3. **The initfs.** `initdata.cpio` from a new `userspace/initfs/`
   staging tree, incbin'd into init.exe (`bootfs_start`/`bootfs_end`).
   Split the test suite out of init into a child binary shipped in the
   archive; port pe.c's loading logic to a userland library so init
   loads that child per ipc doc §5. Init's residual job: spawn, hold
   root, reap.
4. **Design C** when the name registry lands: bootfs server, `boot:`
   namespace, exec-from-namespace library, pivot-by-rebind (§4).

Steps 1–2 are one kernel-stub session; step 3 is a userspace session
with a small kernel deletion at the end; step 4 is its own design
conversation (it owns the name-registry scheme too).

## 6. Implementation notes and deliberate divergences (2026-07-08)

Implementation map (paths per the source-tree reorg of
[source-tree.md](source-tree.md), which landed the same day):
`kernel/src/espfile.c/h` (boot-services file read), `abi/gdos/bootinfo.h`
(the shared struct), `kernel/src/init.c` (`bootinfo_capture` + the
rewritten `init_setup`), `vendor/efi/simple_file_system_protocol.h` +
`vendor/efi/graphics_output_protocol.h` (new protocol headers;
`locate_protocol` got a real type in `efi.h`). Userspace:
`lib/sys/usys.h` (syscall stubs), `lib/sys/cpio.c` (newc reader),
`lib/sys/upe.c` (the §5 loader), `init/init.c`, `init/bootfs.asm`,
`bin/tests/tests.c` (hello.c, renamed and demoted to a mid-tree
process). hello.c, `user_pe_blob.asm`, and the kernel-embedded userspace
build are gone.

- **Entry register: rcx.** `process_spawn_thread`'s `arg` already landed
  in the Win64 first-argument register, so the bootinfo base is a plain
  first parameter to `_start`. No new mechanism.
- **The initfs section is `.rdata`, not a dedicated `.bootfs`.** nasm's
  COFF output merges it into the read-only data section; since discovery
  is the `bootfs_start`/`bootfs_end` symbols, the section name carries
  no meaning and a dedicated one bought nothing.
- **The ESP read buffer is EFI_LOADER_DATA and never freed.** The
  allocator's non-conventional pass marks those pages unusable, so the
  raw init.exe file image (~19 KiB) is permanently reserved, same as the
  kernel image itself. Accepted; revisit only if the initfs makes
  init.exe large.
- **Bootinfo is written through the kernel's own view, then the single
  user view drops to PAGE_R** via the existing `umem_protect` — no new
  kernel surface anywhere in the design; the whole §3 handoff is one
  `umem_alloc` + struct copy + re-flag.
- **upe.c error paths kill but do not reap** a half-built embryo (the
  zombie waits for init's reap loop). Boot-path failures are
  print-and-continue in init, panic-grade in practice.
- **Found and fixed while landing: a latent `group_await` race in the
  test suite** (predates this design, exposed by the new scheduling
  shape). The helper drained every available CQE while hunting one
  event type; two awaited events landing in one batch meant the second
  was consumed by the first call and its await parked forever. It now
  stops draining at the hit and leaves the rest in the CQ. The
  kill-order interleaving that exposed it: the revoked child's exit can
  land before the parent's next `group_await`, batching `KEV_DEAD` and
  the tree channel's `KEV_READY` together.
