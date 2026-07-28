# PCI design: trusted configuration manager, isolated userspace drivers

Status: **planned 2026-07-12.** Companion to
[iommu-design.md](iommu-design.md), [irq-design.md](irq-design.md),
[memory-design.md](memory-design.md), and
[ipc-process-design.md](ipc-process-design.md).

Implementation progress (updated 2026-07-16): the QEMU Q35 substrate through step 8
is implemented, including device-backed ublocks, ECAM enumeration, BAR
delegation, VT-d attachment, MSI/MSI-X setup, and death/restart ordering. The
first step-10 storage slice is also live: `nvmed` exposes a versioned
client-owned shared-block protocol with INFO, READ, and WRITE requests, uses a
driver-owned DMA bounce pool, and services namespace 1 through one NVMe I/O
queue pair. `pcid` performs a one-block read after the death-path restart as an
end-to-end smoke test. The general capability model is now live: init passes
root token bytes to `pcid`, `pcid` sub-grants exact firmware/ECAM/BAR ranges
before `SYS_VM_MAP_DEVICE`, and requester/IRQ tokens cross the driver handoff.
Multi-namespace discovery, concurrent requests,
flush/discard, a name registry, and client-revocation hardening remain.
The fixed v4 setup-page and fused MSI protocol in §§6–8 are planned
replacements for the implemented v3 IRQ handoff; they are not a
compatibility layer.

Govindos follows the seL4 division of labor, without making seL4's full
capability machinery a prerequisite. A trusted userspace service, `pcid`,
owns PCI discovery, configuration space, resource assignment, interrupt
programming, and device lifecycle. A device driver receives only the MMIO
BAR pages, IRQ delivery endpoint, and IOMMU domain interface it needs for
normal operation. It never receives ECAM or legacy PCI configuration ports.

```
                    configuration/lifecycle plane
 init -> pcid ---------------------------------------------------+
          | ECAM, BAR assignment, MSI programming, BME/reset     |
          |                                                      v
          +--- share BAR block ---> driver ---- MMIO ----> PCI function
          +---- arrange IRQ -------> driver <--- event -----+
                                     |                      |
                                     +-- direct IOMMU maps --+-- DMA

 pcid is not on the block-I/O data path.
```

## 0. Decisions log

- **`pcid` alone owns PCI configuration space.** ECAM and x86 CF8/CFC are
  never mapped or delegated to device drivers. Drivers ask `pcid` for reset,
  power-state, PCIe-feature, and interrupt-mode changes over an ordinary user
  channel. This keeps an untrusted driver from moving BARs, enabling ATS,
  changing requester identity features, or forging MSI configuration. In v1
  this split is kernel-enforced by devmem grant tokens: `pcid` receives the
  root from init and maps only exact sub-granted ranges. The kernel also
  retains its independent physical-range safety validation.
- **Drivers receive resources, not configuration authority.** A driver gets
  device-MMIO views for selected BARs, an IRQ route token, and an IOMMU
  requester token. BAR access is direct
  after setup; routine register traffic never passes through `pcid`.
- **Capabilities are the v1 authorization boundary.** The grant/token model
  in [capability-design.md](capability-design.md) gates device mappings, IRQ
  allocation/binding, and requester attachment. Ordinary block sharing still
  carries an already-created BAR mapping to the driver.
- **`pcid` is trusted but outside the kernel.** A compromised `pcid` can
  reconfigure devices, grant the wrong BAR, destroy device availability, and
  corrupt device-owned data. The kernel still rejects mappings of usable RAM,
  kernel/IOMMU/APIC pages, and other non-delegatable ranges, and required-mode
  IOMMU translation still confines DMA. Full protection from a malicious PCI
  manager begins only when configuration authority itself is capability
  partitioned or moved into a smaller trusted component.
- **Firmware assignments first.** V1 accepts firmware-assigned bus numbers
  and BAR addresses after validating type, alignment, host-bridge aperture,
  and overlap. It does not initially rebalance bridges or relocate BARs. BAR
  sizing and assignment are added before hardware that arrives unassigned is
  supported.
- **MSI/MSI-X is programmed by `pcid`.** The kernel allocates the route and
  returns an opaque address/data pair to `pcid`; `pcid` writes MSI config or
  the MSI-X table and then gives the driver IRQ delivery. Drivers never
  receive configuration-space authority to choose a vector or destination.
  Until interrupt remapping exists, a malicious driver can nonetheless have
  its device forge interrupt messages by DMA to the `0xFEE` interrupt
  address range, which bypasses DMA remapping (see iommu-design.md §12) —
  an accepted v1 gap. Interrupt remapping later changes the opaque pair,
  not this split.
- **Bus mastering is last-on, first-off.** `pcid` sets Memory Space Enable as
  needed, but sets Bus Master Enable only after the driver has attached the
  requester to an IOMMU domain, mapped queues/buffers, and installed its IRQ.
  Shutdown masks interrupts and clears BME before IOMMU detach or memory
  unpinning.
- **No kernel PCI enumeration layer.** The kernel parses only the firmware
  descriptions required to validate protected physical ranges, IOMMU
  coverage, and interrupt routing. PCI capability walking, class matching,
  BAR interpretation, and policy remain in `pcid`.

## 1. Goals and non-goals

### Goals

- Discover PCIe functions from ACPI MCFG/ECAM and build a validated topology.
- Give each userspace driver direct access to only its assigned BAR pages.
- Coordinate IOMMU attachment, IRQ setup, device enablement, reset, driver
  death, and teardown in a fail-closed order.
- Keep ECAM/config writes out of untrusted drivers without putting device
  protocols in the kernel.
- Define narrow authorization hooks that pure capabilities can replace later.
- Support the first QEMU Q35 NVMe driver and expose enough metadata for its
  userspace block service.

### Non-goals for v1

- A general capability system or capability transfer over IPC.
- PCI hotplug, surprise removal, SR-IOV, ARI, PASID, ATS, PRI, peer-to-peer
  DMA, resizable BARs, or PCIe IDE.
- Firmware-independent bridge-window and BAR rebalancing.
- Legacy PCI BIOS probing or non-ECAM configuration access by drivers.
- General ACPI interpretation. `_PRT`, power resources, and hotplug methods
  may eventually live in a separate `acpid` service.
- ACS validation. On real hardware, peer-to-peer routing below the IOMMU
  bypasses translation entirely; treating each function as independently
  isolatable is a QEMU-only assumption until topology validation checks
  ACS on every switch and root port along the path (`TODO(ACS)`).
- Making `pcid` harmless if compromised. It is explicitly in the v1 TCB.

## 2. Objects and identifiers

- **PCI segment:** one configuration-space domain, identified by a 16-bit
  segment number.
- **BDF/requester ID:** segment plus bus/device/function, encoded identically
  to `IOMMU_PCI_ID` in the IOMMU ABI.
- **Function record:** `pcid`'s authoritative record containing IDs, class,
  header type, parent bridge, BARs, capabilities, interrupt state, driver
  child PID, and lifecycle state.
- **BAR record:** type (I/O, 32-bit memory, or 64-bit memory), prefetchability,
  assigned base, actual byte size, page-rounded grant extent, and owning
  function.
- **Device-backed ublock:** an ordinary ublock whose pages are a validated
  physical device range instead of buddy RAM. It carries a fixed device
  cache type, is never buddy-owned, executable, or DMA-mappable, and is
  delegated with the ordinary `SYS_VM_SHARE`/`SYS_VM_MOVE`.
- **Assignment:** the temporary v1 relation between one `pcid` child and one
  function. Later this relation is represented by separately delegatable BAR,
  IRQ, and IOMMU-space capabilities rather than one ambient manager decision.

## 3. Required kernel mechanism: device-backed ublocks

Ordinary `SYS_VM_ALLOC` cannot map PCI resources: it allocates zeroed RAM
from the buddy and happens to expose that RAM at its identity address. PCI
needs the inverse operation—authorize an existing physical device range and
install it at that same numeric address with a device cache type.

V1 adds one syscall. Kernel schemes exist for interfaces where the kernel
must deliver events back to userspace; device-memory mapping is synchronous
and strictly user-to-kernel, so it is a plain syscall, not a ring. And
delegation needs no dedicated verbs: the call produces an ordinary
device-backed ublock, so handing it to a driver is `SYS_VM_SHARE` (or
`SYS_VM_MOVE`), and revocation is the existing unshare plus
enumeration-driven process-destruction machinery.

Provisional ABI in `abi/gdosabi/syscall.h`, named in the `SYS_VM_*` family
because the result is a ublock:

| syscall | arguments | effect |
|---|---|---|
| `SYS_VM_MAP_DEVICE` | devmem token pointer/length, flags | create a device-backed ublock over the token's exact, independently validated range at its identity address |

The call is synchronous, bounded, and returns 0 or `SYSERR_*`. Rules:

- any process holding a matching devmem token may call it;
- the block is the delegation unit, fixed at map time: the caller sizes one
  block per extent it intends to share, and separate blocks for pages it
  withholds (an MSI-X table page is its own block);
- ECAM and firmware-table blocks are created non-delegatable: share and
  move on them fail;
- the kernel validates every physical page independently of the caller's
  claim.

A device-backed block differs from a RAM ublock by flags, not machinery: it
is never buddy-owned (`SYS_VM_FREE` retires the mapping without returning
pages to the buddy or writing to device registers), carries a fixed cache
type that every share view inherits, can never be `PAGE_X`, is rejected by
`KIOMMU_MAP_BLOCK`, and is accounted separately from RAM. Everything
else — share views pinning the viewing address space, explicit
unshare, and enumeration-driven destroy ordering — is the ordinary
ublock machinery.

Only `PAGE_R`, `PAGE_W`, `PAGE_UC`, and an allowlisted `PAGE_WC` are accepted;
device mappings are never executable. PCI configuration and ordinary control
BARs use UC. WC is reserved for explicitly allowlisted framebuffer-like
ranges, not chosen by an arbitrary driver. RAM-backed firmware tables
(ACPI) are the one non-device exception: they map read-only WB as
non-delegatable blocks, and only for pages the kernel recorded as
firmware-table ranges. This requires the kernel to keep ACPI and
ACPI-reclaim regions permanently out of the buddy allocator — once
reclaimed as usable RAM they would be rejected by the validator.

The physical-range validator rejects:

- every EFI conventional/usable RAM page, even if currently free;
- kernel image, page tables, ublocks, allocator metadata, bootinfo, and ACPI
  table backing pages not explicitly requested for read-only firmware access;
- IOMMU registers/tables, LAPIC, IOAPIC, HPET, and other kernel-owned MMIO;
- integer overflow, non-page-aligned bases, zero length, and ranges not wholly
  inside one kernel-declared device aperture;
- overlap with any live device block.

BARs smaller than a page are granted as a page-rounded extent only when the
entire page belongs to that BAR's reserved allocation and contains no other
function or protected resource. Otherwise the device is unsupported until a
safe sub-page mediation strategy exists. PCI BAR alignment normally makes
this easy, but the validator must not assume it.

The mapping is installed in the owner's existing per-process page tables;
the kernel identity mapping does not make it user-accessible by itself.
Share views, revocation PTE clears, TLB shootdown ordering, and
parent-driven teardown are the ordinary ublock paths.

For the first Q35 target, the kernel declares the machine's fixed PCI MMIO
apertures after subtracting RAM and its own MMIO reservations. Real hardware
must replace that platform fact with host-bridge windows obtained from ACPI
`_CRS` (whether parsed by a small early component or a later `acpid`) before
arbitrary firmware BAR assignments are accepted. MCFG describes ECAM; it
does not describe the host bridge's allocatable BAR windows.

### Capability boundary (implemented)

`SYS_VM_MAP_DEVICE(token, token_len, flags)` maps the exact range in a live
devmem grant, with flags narrowed by both the grant and the platform safety
validator. `pcid` creates those grants through `KCAP_SUBGRANT`; device blocks
then use ordinary `SYS_VM_SHARE`/`SYS_VM_MOVE` delegation.

Nothing in `pcid`'s enumeration logic or a driver's MMIO access changes.
The kernel stores revocation anchors, while token bytes remain ordinary IPC
data; there is still no per-process capability table.

## 4. Discovery and topology

`pcid` receives the ACPI RSDP address through bootinfo. It maps ACPI tables
read-only through `SYS_VM_MAP_DEVICE`, validates checksums and lengths, and
finds MCFG. The kernel independently knows protected ACPI/MCFG ranges well
enough to validate these mappings; userspace parsing remains authoritative
for enumeration.

For each MCFG allocation, `pcid` validates segment and bus ranges, maps the
ECAM window UC into itself, then scans buses. ECAM address calculation is:

```
ecam + ((bus - start_bus) << 20) + (device << 15) + (function << 12)
```

Enumeration reads vendor ID first and skips `0xFFFF`. It honors multifunction
header bits, records bridges, and recursively scans only firmware-configured
secondary/subordinate bus ranges in v1. Loops, duplicate BDFs, malformed
bridge ranges, and ECAM arithmetic overflow reject that branch.

For each function, `pcid` records:

- vendor/device and subsystem IDs;
- class/subclass/programming interface and revision;
- header type and parent bridge path;
- command/status and interrupt pin/line;
- BAR type/base and validated size;
- standard capability list with loop and bounds detection;
- MSI, MSI-X, PCIe, power-management, and FLR support relevant to lifecycle.

Extended capabilities are not needed by the first NVMe path. When parsed,
their linked list receives the same alignment, range, and cycle checks.

## 5. BAR policy

V1 preserves firmware BAR assignments. Before accepting a memory BAR,
`pcid` checks that:

- the encoding is valid and a 64-bit BAR has its high dword;
- base and size do not overflow and satisfy BAR alignment;
- the entire range lies in a host-bridge MMIO aperture and outside RAM;
- it does not overlap ECAM, kernel-owned MMIO, another accepted BAR, or a
  bridge window that cannot forward it;
- every upstream bridge already forwards the range.

BAR size probing writes all ones and temporarily destroys the BAR value. It
is permitted only while the function is unbound, Memory Space Enable and Bus
Master Enable are clear, and interrupts are disabled. Config writes are
32-bit, so a 64-bit BAR cannot be rewritten atomically; the save/restore is
safe only because Memory Space Enable is clear, which keeps the transient
value from ever participating in address decoding. V1 probes every
candidate function before validating and
granting its firmware assignment; it must never probe a live function.

The driver receives only the BAR pages selected by its device-class binding.
The first NVMe driver receives its controller register pages, not ECAM and
not unrelated expansion-ROM or vendor BARs. `pcid` computes the MSI-X table
and pending-bit-array extents from BIR/offset/size and withholds their pages
from the driver. If an MSI-X table page also contains controller registers
the driver needs, v1 uses MSI in configuration space instead; if neither is
possible, that function is unsupported until interrupt remapping or safe
sub-page mediation exists. `pcid` retains its own view of the table pages to
program and mask them.

I/O-port BARs require a range-restricted I/O-port mechanism analogous to
seL4 `IOPort` capabilities. They are deferred; v1 drivers require memory
BARs.

## 6. Driver binding and userspace protocol

`pcid` contains the initial static match table. For NVMe it matches class
`01/08/02`; later it can load declarative matches from initfs. Broad generic
matches lose to exact vendor/device matches, and one function binds once.

`pcid` is the parent of every hardware driver it manages. It creates
the child, maps device blocks for the selected BAR extents and shares
them into the child, creates a shared setup page, moves the image/stack,
and starts the child. The fixed v4 page is both the start record and the
readiness reply; token fields are authority bytes, while numeric ids are
only identity/correlation:

```c
#define PCI_DRIVER_START_VERSION   4
#define PCI_DRIVER_MAX_BARS        6
#define PCI_DRIVER_MAX_IRQ_ROUTES 32

struct pci_driver_bar {
  uint64_t base;
  uint64_t length;
  uint32_t bar_index;
  uint32_t flags;
};

struct pci_driver_start {
  uint32_t version;
  uint32_t n_bars;
  uint64_t requester_id; // diagnostic/ring-scoped detach name
  uint64_t function_id;  // pcid-local cookie for later requests
  struct pci_driver_bar bars[PCI_DRIVER_MAX_BARS];
  _Atomic uint32_t state;
  uint32_t n_irq_routes;
  struct cap_token iommu_token;  // narrowed requester authority
  struct cap_token irq_wildcard; // copied shared MSI allocate+bind authority
  struct cap_token irq_routes[PCI_DRIVER_MAX_IRQ_ROUTES]; // driver reply
  uint64_t service_channel;
};

#define PCI_DRIVER_QUEUES_READY 1
#define PCI_DRIVER_LIVE         2
#define PCI_DRIVER_STOP         3
#define PCI_DRIVER_DMA_STOPPED  4
```

`pcid` zeroes the whole page, fills every immutable field through
`irq_wildcard`, then starts the child. The driver is the only writer of
`n_irq_routes` and `irq_routes[]`; unused entries remain zero. State
transitions are sequential: driver `QUEUES_READY`, `pcid` `LIVE`,
`pcid` `STOP`, driver `DMA_STOPPED`. Every producer release-stores
`state` only after its associated fields/actions are complete; every
consumer acquire-loads it before reading them. Both sides reject an
unknown version, `n_bars > PCI_DRIVER_MAX_BARS`, or
`n_irq_routes > PCI_DRIVER_MAX_IRQ_ROUTES`.

The driver receives shared BAR views rather than a devmem token, so it
cannot create arbitrary device mappings. Its start record carries a
narrowed requester token and a byte-for-byte copy of `pcid`'s wildcard
IRQ token. The latter is intentionally not narrowed in v1: all trusted
drivers may use it only for fused MSI allocate+bind. Numeric requester
identity remains present for diagnostics and ring-scoped detach, not as
attach authority.

Driver-to-`pcid` control requests include readiness, orderly stop, FLR/reset,
power-state change, and diagnostic config reads from an allowlisted set.
There is no generic config-write request. Unknown operations or attempts to
access another function fail and are logged.

## 7. IRQ setup

MSI route allocation is fused with ring binding and belongs to the
*driver*; hardware programming stays with `pcid`
([irq-design.md](irq-design.md)):

1. `pcid` copies its wildcard IRQ token into
   `pci_driver_start.irq_wildcard`.
2. During its own setup the driver submits fused `KIRQ_MSI` ops on its
   IRQ ring: each allocates a free vector route, binds it to that ring
   atomically, and yields a concrete route token. It stores successful
   tokens densely in `irq_routes[]`, stores `n_irq_routes`, then
   release-stores `PCI_DRIVER_QUEUES_READY` to `state`.
3. `pcid` acquire-observes `QUEUES_READY`, rejects
   `n_irq_routes > PCI_DRIVER_MAX_IRQ_ROUTES`, and copies each token
   into its own IRQ-ring block for the offset-based query ABI. This is
   the complete wire contract; there is no separate `IRQ_READY`
   rendezvous or variable-length control message.
4. `pcid` derives each route's address/data pair with `KIRQ_MSI_ADDR` —
   token-authenticated and generation-checked, so a relayed-wrong or
   stale token cannot aim a device at someone else's vector — masks the
   function's MSI/MSI-X source, then programs it: for MSI the capability
   in ECAM, for MSI-X the table BAR it maps into itself, with the
   required ordering.
5. `pcid` enables MSI/MSI-X and unmasks only after programming completes.

Routes die with the driver's IRQ ring — release or ring destruction,
including `present` routes. Pin routes stay masked until a next
claimant reprograms; MSI sources follow the mask/reset-before-ring-free
ordering in §8, subject to the accepted pre-IR stale-source caveat. Thus
there is no unbound-allocated state and no process-destroy route sweep.
Authorization is token provenance throughout;
direct-child PID provenance plays no part.

If the driver cannot allocate all requested routes, it releases every
route already recorded before reporting failure; process/ring teardown
is the backstop and reaps any slot it missed. If `pcid` rejects the
reply or programming fails partway, it keeps every source masked,
clears BME, kills the driver, and lets IRQ-ring destruction release all
routes. Tokens left in the setup page retain no slot.

V1 trusts drivers not to exhaust the fixed pool because they all hold
the wildcard token and there is no quota. Interrupt remapping later
provides a larger source-indexed namespace, reducing this risk while
also fencing stale MSI programming.

INTx is deferred entirely: `_PRT` routing requires AML interpretation,
which is a stated non-goal until an `acpid` exists. V1 devices must support
MSI or MSI-X. Drivers never program MSI config themselves, even before
interrupt remapping.

## 8. IOMMU, enablement, and teardown

Successful discovery does not enable DMA. The start sequence is:

```
pcid:   validate function/BARs; keep BME clear; mask interrupts
pcid:   set Memory Space Enable for the validated BARs
pcid:   spawn driver; share selected BAR blocks and setup channel
driver: create IOMMU ring/domain
driver: map queue and bounce-buffer ublocks
driver: DEVICE_ATTACH(requester token)
driver: fuse MSI routes onto its IRQ ring (KIRQ_MSI)
driver: initialize disabled device queues/register image
driver: write n_irq_routes + route tokens; release-store QUEUES_READY
pcid:   KIRQ_MSI_ADDR per token; program MSI/MSI-X masked
pcid:   set Bus Master Enable
pcid:   enable/unmask interrupt source
pcid -> driver: LIVE
driver: enable controller and serve block requests
```

If any step fails, `pcid` masks interrupts, leaves/clears BME, revokes
BAR block shares as needed, and kills the child, runs the
enumeration-driven teardown choreography, then calls
`SYS_PROC_DESTROY`. It never enables an incompletely isolated function.

Orderly stop and driver-death recovery use the reverse safety order:

```
pcid:   mask MSI/MSI-X and device interrupt sources
pcid:   request device quiesce, then FLR/reset if necessary
pcid:   clear Bus Master Enable and read back command/status
pcid -> driver: DMA_STOPPED             // omitted if driver is dead
driver: DEVICE_DETACH; unmap; destroy domain
pcid:   revoke the BAR block shares (routes die with the driver's IRQ ring)
pcid:   enumerate/tear down driver resources; PROC_DESTROY(driver)
```

Clearing BME is not proof that all earlier posted transactions have drained;
the device-specific reset/quiesce rule and IOMMU context invalidation provide
the boundary. If `pcid` cannot reset a wedged device, it still clears BME and
asks the kernel to detach the requester. The IOMMU fails subsequent DMA, and
mapped ublocks remain pinned until detach/invalidation completes.

Bridge forwarding bits are reference-counted by `pcid`. Enabling one child
may require Memory Space Enable, and where architecturally required Bus
Master Enable, along its upstream path. A bridge bit is cleared only when no
enabled descendant needs it; a function assignment never grants the driver
authority over the bridge.

## 9. Failure, restart, and ownership

The intended process tree is `init -> pcid -> hardware drivers`.
Therefore `pcid` receives child-death events and owns cleanup. A dead
driver cannot unmap its BARs or detach itself, so `pcid` first performs
hardware quiescence, then uses the enumeration/coercion verbs to remove
IOMMU mappings, IRQ-ring blocks, device-block views, and ordinary
ublocks in the required order. `SYS_PROC_DESTROY` only frees the empty
process body.

If `pcid` dies, init must treat all of its driver descendants as failed:

1. kill the subtree;
2. have init walk the dead descendants post-order, freeing IRQ-ring
   blocks (which release routes) and explicitly revoking device-block
   views;
3. rely on IOMMU default-deny/context teardown to contain DMA;
4. restart `pcid`, which re-enumerates only after the old subtree is gone;
5. before any BAR probing, the restarted `pcid` walks configuration space
   clearing Bus Master and Memory Space Enable and masking MSI/MSI-X on
   every function — devices may still be enabled from before the crash,
   the kernel cannot touch config space, and §5's probe rules forbid
   sizing a live function.

The kernel cannot perform device-specific FLR after `pcid` dies because it
does not understand PCI capabilities. This is a v1 availability limitation:
DMA remains confined, but a device may remain wedged until platform reset.
A tiny configuration guardian or capability-backed recovery service can
later reduce this trusted-lifecycle gap.

## 10. Cache and alias rules

All CPU mappings of a physical device page must use one compatible cache
type system-wide. This is structural now that device memory is
block-shaped: `SYS_VM_MAP_DEVICE` rejects overlap with any live device
block, so exactly one block owns each physical device page, and every
authorized alias is a share view of that block inheriting its recorded
cache type. The kernel never scans every process to rewrite PTEs.

Ordinary processes have no user-accessible mappings of device pages outside
this path. Kernel mappings of the same page must use the block's type too.
ECAM, ordinary BARs, and IOMMU registers are UC; coherent DMA buffers
remain WB RAM and are never device blocks.

Revocation removes only the target process's views and performs the normal
targeted TLB shootdown. It does not change the physical page's cache type
while the block or other views remain.

## 11. Security invariants

1. In the supported boot graph only `pcid` maps ECAM or PCI configuration
   ports — convention in v1, kernel-enforced once capabilities land.
2. No device block includes usable RAM, kernel-owned MMIO, IOMMU state, or a
   range outside the kernel-declared device apertures.
3. All live CPU mappings of one device page use compatible cache types.
4. A driver receives only explicitly selected BAR pages; page rounding never
   exposes another function or protected resource.
5. BME is set only after a live IOMMU context and required DMA mappings exist.
6. IRQ sources are masked before route destruction, reset, or driver
   process destruction.
7. BARs are never sized or relocated while a function is live.
8. Driver death cannot free DMA-pinned memory before context removal and
   IOTLB invalidation.
9. Future capability checks replace the marked authorization hooks; they do
   not coexist with an ambient bypass path.

## 12. Verification plan

### Parser and policy tests

- Valid, truncated, overlapping, and overflowed MCFG allocations.
- ECAM scan with absent, single-function, multifunction, and bridge devices.
- Cyclic/malformed standard capability lists.
- 32-bit, 64-bit, prefetchable, I/O, zero, and malformed BARs.
- BAR overlap with RAM, ECAM, kernel MMIO, another BAR, and bridge windows.
- Device-block overlap rejection and sub-page sharing rejection.
- Share/move of non-delegatable (ECAM, firmware-table) blocks fails.

### QEMU Q35 tests

1. Enumerate Q35 root ports and NVMe from MCFG without CF8/CFC.
2. Confirm the NVMe driver can access its controller BAR but faults on ECAM
   and on an unrelated BAR address.
3. Have the driver call `SYS_VM_MAP_DEVICE` on RAM, kernel MMIO, and a
   range overlapping a live device block; all fail. Have `pcid` attempt to
   share its ECAM block; that fails too.
4. Attempt to map conventional RAM and IOMMU registers; both fail.
5. Verify BME remains clear until IOMMU attach, queues, and IRQ are ready.
6. Complete one NVMe command using MSI/MSI-X programmed only by `pcid`.
7. Kill the driver during I/O; observe mask/reset/BME-clear before
   bounded enumerated IOMMU and memory teardown.
8. Restart the driver and verify no stale IRQ route, device-block view,
   requester context, or BAR mapping survives.
9. Kill `pcid`; verify the subtree dies and no DMA reaches unpinned memory.

## 13. Suggested implementation order

1. Add a checked ACPI table lookup shared with IOMMU work; parse MCFG and
   reserve ECAM/device apertures in the kernel's physical-range validator.
2. Implement device-backed ublocks: no buddy ownership, fixed inherited
   cache type, never `PAGE_X` or DMA-mapped, free without touching device
   registers; test rejection of RAM and protected MMIO.
3. Implement token-authorized `SYS_VM_MAP_DEVICE` and non-delegatable
   ECAM/firmware blocks; delegation and revocation ride the existing
   share/move machinery.
4. Build `pcid`: ACPI/MCFG parser, ECAM mapping, enumeration, topology, and
   capability-list validation (prerequisite: add the ACPI RSDP address to
   bootinfo). Probe inactive functions to validate and then
   preserve valid firmware BAR assignments.
5. Add driver matching and the parent/child setup protocol; share the NVMe
   controller BAR block and prove ECAM is inaccessible to the child.
6. Land required-mode VT-d through IOMMU design steps 1–8; attach NVMe by the
   temporary bare-requester hook.
7. Adjust the IRQ MSI milestone so `pcid` allocates/programs routes and a
   direct child binds delivery; keep the future IRQ-capability hook.
8. Implement the start/stop/death state machine and BME/bridge reference
   counts; run kernel-memory-DMA, other-process-memory-DMA and kill-during-I/O tests.
9. Add safe BAR sizing/allocation and bridge-window programming when real
   hardware requires firmware-independent assignment.
10. Build the NVMe block service, then the partition reader and FAT32 service
    on top of its userspace block protocol.
11. When govindos adopts pure capabilities, replace the four marked
    authorization hooks—PCI control, device memory, IRQ route, and IOMMU
    requester—without redesigning the data paths.

Steps 1–8 are the platform/driver substrate required before NVMe is a safe
userspace block device. Partition parsing and FAT32 need no additional kernel
mechanism; they are ordinary userspace consumers once the block protocol and
device-death behavior are defined.
