# IOMMU design: required DMA isolation for userspace drivers

Status: **planned 2026-07-12, revised for the `pcid`/driver authority split
2026-07-12; `iommu=required` is the only initial mode.**
Companion to [memory-design.md](memory-design.md),
[ipc-process-design.md](ipc-process-design.md), and
[irq-design.md](irq-design.md). PCI ownership and device delegation are
specified by [pci-design.md](pci-design.md). The first backend is Intel VT-d
on x86_64;
the architecture-neutral core is deliberately shaped for AMD-Vi and Arm
SMMUv3 backends later.

Implementation progress (updated 2026-07-16): steps 1-10 and the QEMU portion of step
11 are implemented. `nvmed` maps its admin queue, I/O queue pair, PRP-list
page, Identify pages, and eight-page bounce pool as one driver-owned IOMMU
block. Client channel pages are copied to/from that pool and are never DMA
mapped. QEMU verifies mapped DMA, normalized malicious-PRP faults,
death/restart cleanup, and a block-protocol read on the replacement driver.
`KIOMMU_DEVICE_ATTACH` now verifies a per-requester capability token issued
by `pcid`; detach remains authorized by the existing ring/domain attachment.
Broader malicious-I/O tests and real-hardware scope/RMRR support remain.

The IOMMU is the DMA-side counterpart of the CPU page tables. CPU page
tables stop a driver thread from touching memory it was not granted; an
IOMMU domain stops the device controlled by that driver from doing the same
thing through DMA. This is what makes a bus-mastering userspace driver a
real isolation boundary instead of kernel-equivalent trusted code.

Govindos' identity layout makes the first useful implementation unusually
small at the API boundary:

```
CPU:     process VA ------------------------------> physical page
Device:  same numeric IOVA --> IOMMU domain table -> physical page
                              |
                              `-> every ungranted page faults
```

For v1, `IOVA == PA == the ublock base plus offset`. The IOMMU tables still
earn their keep: they are a per-device allow-list, not an address-placement
mechanism. A future non-identity IOVA allocator does not change the domain,
pinning, or device-assignment model below.

## 0. Decisions log

- **Required means required.** If firmware does not describe a supported
  IOMMU, a DMA-remapping unit cannot be initialized, or a requested device
  is not covered by one, govindos does not enable that device's bus-master
  bit. Failure during early global initialization is fatal before init is
  spawned. There is no passthrough or trusted-driver fallback in v1.
- **Default deny before userspace.** The kernel enables DMA translation with
  an empty device-to-domain map before any govindos userspace driver may
  enable bus mastering. A requester without a present context faults. The
  supported-platform contract also requires firmware to hand off with DMA
  quiesced; the QEMU target satisfies that contract. Firmware-originated
  pre-handoff DMA is outside what a post-handoff kernel can repair.
- **Drivers operate their own domains directly.** Each driver creates one
  IOMMU control ring, creates domains owned implicitly by itself, maps and
  unmaps only its own ublocks, attaches unclaimed requester IDs, and receives
  its own fault events. There is no `pcid` proxy on the DMA data path.
- **Device attachment is capability-gated.** `DEVICE_ATTACH` consumes a
  live per-requester token from the ring block, then preserves the existing
  exclusive-attachment check. `DEVICE_DETACH` is ring/domain scoped so
  prospective token revocation cannot prevent cleanup.
- **`pcid` owns assignment and device lifecycle.** It exclusively owns
  ECAM/configuration, identifies devices, grants BAR access by sharing
  device-backed ublocks, programs MSI/MSI-X, enables bus mastering only
  after driver setup, and resets/disables a dead driver's device before
  asking the kernel to reap that driver. It does not create domains, map
  routine DMA buffers, or sit on the I/O data path.
- **Whole owned ublocks are the v1 DMA-grant unit.** A domain may map only a
  block owned by the ring's process, at the block's identity IOVA. No raw
  physical ranges, shared-in blocks, subranges, client zero-copy, or MMIO
  may be DMA-mapped. NVMe initially uses driver-owned bounce buffers.
- **A mapped block is pinned.** It cannot be freed, moved, returned to the
  buddy, or reclaimed by reap until its IOMMU PTEs are gone, the IOTLB has
  been invalidated, and the pin has been released. CPU mapping lifetime and
  DMA mapping lifetime cannot disagree.
- **All kernel-channel commands stay bounded.** One map or unmap SQE
  installs or removes every leaf of one block in a single invocation. This
  is bounded without a resumable cursor because block page counts are
  capped and hugepage-backed extents collapse to second-level superpage
  entries (reusing the paging.c walk mechanics), keeping the worst case on
  the order of one table page of leaf writes. No device-controlled table
  walk turns one doorbell into unbounded kernel work.
- **One domain per driver, one assignment per PCI requester.** Multiple
  functions may be attached to a domain when they intentionally share a
  driver, but a requester ID belongs to at most one domain. Real-hardware
  isolation groups are deferred; v1 supports QEMU devices whose requester
  IDs are independently remapped.
- **The initial backend is VT-d legacy mode.** Scalable mode, PASID, PRI,
  ATS, shared virtual addressing, queued invalidation, and nested
  translation are deferred. Register invalidation is sufficient for the
  first NVMe driver. Second-level entries use 2 MiB superpages for
  hugepage-backed extents when the unit reports support (SLLPS), with
  4 KiB leaves as the general case.
- **DMA remapping and interrupt remapping are separate milestones.** DMA
  remapping lands first. Until VT-d interrupt remapping exists, `pcid`
  retains PCI configuration authority and programs MSI/MSI-X using the
  opaque pair returned by the IRQ scheme. DMA isolation is enforced;
  interrupt isolation is not: in VT-d legacy mode a device DMA write to
  `0xFEE0_0000`–`0xFEEF_FFFF` is treated as an interrupt request and
  bypasses second-level translation, so a malicious driver can have its
  device deliver arbitrary vectors to arbitrary APIC destinations. This
  spoofing gap is accepted until interrupt remapping lands.
- **An IOMMU fault is contained, not a kernel panic.** Faults coalesce into
  events on the owning driver's IOMMU ring. The domain remains installed, so
  the offending device is still confined. The driver may quiesce and tear
  down voluntarily; after driver death, `pcid` disables bus mastering/resets
  the function and parent-driven reap removes the domain incrementally.

## 1. Goals and non-goals

### Goals

- Prevent a PCI bus master assigned to one userspace driver from reading or
  writing kernel memory, another process's memory, or ungranted blocks of
  its own process.
- Make device assignment, DMA mapping, driver death, and process reaping
  compose with the existing ublock and process-tree lifetime rules.
- Let drivers control their DMA address spaces without exposing remapping
  tables or IOMMU registers to userspace.
- Preserve the kernel-channel design law: commands are bounded and
  non-blocking; asynchronous hardware faults arrive as events.
- Provide one architecture-neutral core and ABI with replaceable VT-d,
  AMD-Vi, and SMMUv3 backends.
- Make incorrect DMA reproducibly fault under QEMU, with enough diagnostics
  to identify the requester, address, access direction, and reason.

### Non-goals for v1

- Booting or enabling DMA devices without a usable IOMMU.
- Protecting against malicious firmware or DMA completed before kernel
  handoff.
- PCI hotplug, SR-IOV, peer-to-peer DMA, or general IOMMU groups.
- Mapping arbitrary userspace pages, scatter/gather virtual memory, or
  assigning user-chosen IOVAs.
- Direct DMA into client-owned/shared channel blocks.
- Demand paging, PRI, PASID, ATS, or Shared Virtual Addressing.
- Interrupt remapping. Its table and MSI format will be added behind the
  existing opaque `KIRQ_MSI` result later.
- Authenticating device claims. The ABI reserves the authorization point,
  but v1 uses exclusive first claim. Later, ordinary unforgeable
  device/group capabilities replace the bare ID rather than adding PID, UID,
  or privileged-process policy.

## 2. Terms and identifiers

- **Remapping unit:** one hardware IOMMU register block. A platform may have
  several, each covering some requesters.
- **Requester ID:** the identity presented by a DMA transaction. For PCI it
  is segment plus bus/device/function (BDF). For SMMUv3 it resolves to a
  StreamID.
- **Domain:** a set of DMA translations and permissions shared by the
  devices intentionally assigned to one driver.
- **Mapping:** one driver-owned ublock installed in one domain.
- **DMA pin:** the reference preventing that ublock from being recycled
  while the mapping or a stale hardware translation may exist.
- **Domain cookie:** a nonzero u64 chosen by a driver to name one of its
  domains in its control ring. It is unique while that domain exists and
  avoids requiring
  result-bearing command completions in the current kring ABI.

The shared PCI requester encoding is:

```c
// Bits 31:16 segment, 15:8 bus, 7:3 device, 2:0 function.
#define IOMMU_PCI_ID(segment, bus, device, function) \
  (((uint64_t)(segment) << 16) | ((uint64_t)(bus) << 8) | \
   ((uint64_t)(device) << 3) | (uint64_t)(function))
```

The kernel validates `device < 32` and `function < 8`; callers may not use
the unused upper bits. This numeric ID is an ABI name, not an authorization
token.

## 3. Boot and fail-closed transition

IOMMU initialization runs after the EFI memory map and ACPI root are known,
but before init or any other userspace process is spawned.

1. Locate and checksum the architecture's firmware table: DMAR for VT-d,
   IVRS for AMD-Vi, IORT (or a platform Device Tree) for SMMUv3.
2. Parse every remapping unit, its segment, device scopes, include-all
   coverage, and firmware-reserved DMA regions.
3. Map each unit's register pages kernel-only and UC before user address
   spaces are cloned.
4. Read and validate hardware capabilities. Unsupported address widths,
   table formats, or invalidation mechanisms are fatal in required mode.
5. Allocate zeroed root/device tables. All requester contexts begin absent.
6. Program the hardware root pointer, perform the required global context
   and IOTLB invalidations, enable translation, and wait with a fixed
   timeout for the enabled status.
7. Enable fault recording. Fault interrupts may be deferred initially, but
   records must be polled and dumped during bring-up.
8. Only after every remapping unit is live may init start PCI discovery and
   drivers. `pcid` enables a function's Bus Master Enable bit only after the
   driver reports that its domain, device context, queues, and buffers are
   ready.

The empty context map is deliberate: a firmware-left-active requester will
fault instead of gaining an identity domain. Required mode never installs a
global passthrough domain.

The first QEMU target uses `-machine q35 -device intel-iommu`. The existing
development machine must move from the implicit legacy PC machine to Q35
when the VT-d backend lands. An NVMe test device is attached only after the
default-deny boot test passes.

## 4. Architecture-neutral kernel core

The core owns policy-independent objects and delegates table formats and
register operations to one backend per remapping unit.

```c
struct iommu_unit;
struct iommu_domain;
struct iommu_mapping;

struct iommu_device_id {
  uint16_t segment;
  uint8_t bus;
  uint8_t devfn;
};

enum iommu_perm {
  IOMMU_PERM_DEVICE_READ  = 1u << 0, // device reads RAM
  IOMMU_PERM_DEVICE_WRITE = 1u << 1, // device writes RAM
};

struct iommu_backend_ops {
  bool (*covers)(struct iommu_unit *, struct iommu_device_id);
  uint64_t (*domain_init)(struct iommu_domain *);
  uint64_t (*attach)(struct iommu_domain *, struct iommu_device_id);
  uint64_t (*detach)(struct iommu_domain *, struct iommu_device_id);
  uint64_t (*map_range)(struct iommu_domain *, uint64_t iova,
                        uint64_t phys, uint64_t pages,
                        uint32_t permissions);
  uint64_t (*unmap_range)(struct iommu_domain *, uint64_t iova,
                          uint64_t pages);
  uint64_t (*invalidate_domain)(struct iommu_domain *);
  void (*domain_destroy)(struct iommu_domain *);
};
```

Names and exact signatures may change during implementation; the required
separation may not. The common layer is responsible for:

- endpoint/domain/device/mapping lookup and ownership validation;
- opaque hardware domain-ID allocation with generation-safe reuse;
- selecting the unit that covers a requester;
- enforcing one assignment per requester;
- validating ublock ownership, size, permissions, and state;
- acquiring and releasing DMA pins;
- single-invocation whole-block map/unmap within the leaf cap;
- domain and global resource bounds;
- fault normalization and event delivery;
- process death/reap integration.

The backend is responsible for:

- firmware scope interpretation specific to its architecture;
- root, context/stream, and second-level entry formats;
- register programming and capability checks;
- memory barriers and hardware invalidation;
- fault-register or event-queue decoding.

All table memory is allocated from kernel RAM, page-aligned, zeroed, and
never made `PAGE_U`. Hardware tables contain physical addresses, which are
numerically identical to their kernel pointers under the identity layout.

### Fixed initial bounds

V1 uses compile-time per-process and global caps so a hostile driver cannot
consume unlimited kernel memory:

- `IOMMU_MAX_DOMAINS`
- `IOMMU_MAX_DEVICES`
- `IOMMU_MAX_MAPPINGS`
- `IOMMU_MAX_MAPPING_LEAVES` (per block, after superpage collapse)

Exhaustion returns `SYSERR_NOMEM`. These are implementation limits, not
ABI promises.

## 5. Direct driver control and the future authorization hook

The current kernel has no general capability objects, and the IOMMU is not a
reason to block on designing them. There is no IOMMU-root scheme, PID
appointment, credential check, privileged-process flag, or global manager
endpoint in v1.

Scheme `-6` may be created once per process. The ring owner is implicitly
the owner of every domain created through that ring; no command accepts a
target PID. A process may map only exact ublocks it owns. Domains and device
claims are scoped to the endpoint internally, so a cookie supplied on one
ring cannot name or mutate an object created through another ring.

A process may claim any currently unassigned requester ID. The kernel
enforces exclusive claims and hardware coverage but deliberately applies no
device policy in v1. Claiming another driver's intended device is denial of
service, not a memory-isolation bypass: the claimant can map only its own
blocks into the resulting domain. Recovery is the ordinary process
mechanism: `SYS_PROC_KILL` on the claimant (subtree-scoped, so possibly
escalated to init) and its reap detach the contexts and return the
requester IDs to the unclaimed pool. The kernel should report the claiming
PID in serial diagnostics so a squatted requester can be identified. V1 accepts only PCI endpoint functions the
backend can resolve directly; bridge assignment, requester aliases, and
multi-function/IOMMU groups remain unsupported rather than being guessed.

Keep authorization isolated in one function, conceptually
`iommu_authorize_attach(process, requester)`. It returns true for every
well-formed unclaimed requester in v1. When pure capabilities land, the SQE's
requester field becomes a local device/group-capability handle, that function
resolves it to the hidden requester set, and the remaining attach path is
unchanged. This is a planned replacement point, not an implementation
prerequisite or a second temporary security framework.

The control ring may be destroyed voluntarily only after all domains,
devices, mappings, and pins have been removed. A live endpoint with
resources makes block free return `SYSERR_EXIST`.

Process death is different: the ring and domains remain on the zombie, with
all mapped blocks pinned, until its parent reaps it. The first IOMMU reap
steps remove device contexts and complete their invalidations; later bounded
steps remove mapping leaves and pins, destroy domains, and finally allow the
control block and ordinary ublocks to be reclaimed. No device can reach
recycled memory during that sequence.

## 6. Driver ring ABI

The ABI lives in `abi/gdosabi/kring_iommu.h`. Every command completion is the
ordinary kring completion: it echoes `op`, `a`, and `b`, with `status` equal
to zero or `SYSERR_*`. Domain names are driver-selected cookies scoped to
the ring, so no command needs to return a newly allocated handle.

```c
#define KSCHEME_IOMMU ((int64_t)-6)

#define KIOMMU_DOMAIN_CREATE  1
// a = nonzero domain cookie, b = 0, c = 0; owner is the calling process

#define KIOMMU_DOMAIN_DESTROY 2
// a = domain cookie; requires no devices and no mappings

#define KIOMMU_DEVICE_ATTACH  3
// a = offset of {domain cookie, token offset/length, fault-event cookie}
//     within this ring block

#define KIOMMU_DEVICE_DETACH  4
// a = domain cookie, b = IOMMU_PCI_ID(...)

#define KIOMMU_MAP_BLOCK      5
// a = domain cookie, b = exact owned ublock base, c = IOMMU_PERM_*

#define KIOMMU_UNMAP_BLOCK    6
// a = domain cookie, b = exact mapped ublock base

#define KEV_IOMMU_FAULT KEV(6)
// a = device's fault-event cookie, b = faulting IOVA,
// status = normalized IOMMU_FAULT_* reason with access direction encoded
```

Common error meanings:

- `SYSERR_INVAL`: malformed ID/flags; unknown domain, device, or
  block; block is not owned by the domain's driver; unsupported permission;
  device is not covered by a supported unit.
- `SYSERR_EXIST`: duplicate cookie, assignment, mapping, or an object whose
  state prevents the requested transition.
- `SYSERR_NOMEM`: a fixed resource bound or table allocation was exhausted,
  including a block whose leaf count exceeds `IOMMU_MAX_MAPPING_LEAVES`.
- `SYSERR_DEAD`: the control endpoint or relevant device/domain state died.

### Required operation order

For starting a device:

```
pcid: validate BARs; set Memory Space Enable; keep Bus Master Enable clear
driver: create IOMMU ring
driver: DOMAIN_CREATE
MAP_BLOCK(queue memory, device read|write)
MAP_BLOCK(data buffers, permissions)
DEVICE_ATTACH(BDF)
driver -> pcid: domain/device/queues ready
pcid: program MSI/MSI-X
pcid: set PCI Bus Master Enable
pcid -> driver: device is live
```

For stopping or recovering it:

```
driver -> pcid: request device stop
pcid: reset/disable the device
pcid: clear Bus Master Enable
pcid -> driver: DMA stopped
DEVICE_DETACH(BDF)
UNMAP_BLOCK(...) for every mapping
DOMAIN_DESTROY
destroy IOMMU ring
```

`DEVICE_ATTACH` publishes a present hardware context only after all prior
table writes and invalidations complete. `DEVICE_DETACH` removes the context
and completes its context/IOTLB invalidations before returning.
Any DMA racing detach faults; it cannot escape the old domain.

The kernel cannot determine from an IOMMU register whether device-specific
engines are quiescent. Clearing bus mastering before detach is therefore a
trusted `pcid` responsibility. The security fallback remains sound if it
gets the order wrong: the absent context blocks later DMA, though the
device may fault or wedge.

On unexpected driver death the parent follows the same hardware order as
far as possible, then calls `SYS_PROC_REAP`. The reaper performs
`DEVICE_DETACH`, unmap, and domain destruction on behalf of the dead process
in bounded steps; no userspace thread needs to survive to clean up the
IOMMU objects.

## 7. Mapping state and bounded work

`KIOMMU_MAP_BLOCK` accepts an exact ublock base. One invocation:

1. resolves the ring owner's live process;
2. finds the block in that process's owned-block list;
3. rejects kernel-channel, device-memory, shared-in, already-mapped, and
   over-cap blocks;
4. allocates a mapping record and takes one DMA pin;
5. installs every leaf, reusing the paging.c walk mechanics: identity
   IOVA == PA preserves alignment, so a hugepage-backed 2 MiB-aligned
   extent becomes one second-level superpage entry when the unit reports
   SLLPS support, with 4 KiB leaves otherwise;
6. publishes the table writes and performs one domain IOTLB invalidation.

There is no resumable cursor: `IOMMU_MAX_MAPPING_LEAVES` caps the work at
roughly one table page of leaf writes after superpage collapse. QEMU's
`intel-iommu` reports Caching Mode = 1, which requires invalidation even
for not-present-to-present transitions before the mapping is relied on;
the single end-of-map invalidation satisfies both that and real hardware.

The pin is taken before the first PTE becomes present. If leaf installation
fails partway (table allocation exhaustion), the installed leaves are
removed, the invalidation still runs, and the pin is released before the
error returns; a failed map leaves no hardware-visible state.

`KIOMMU_UNMAP_BLOCK` clears every leaf, performs the final IOTLB
invalidation, and only then releases the pin and retires the mapping
record. Only then may the block be recycled.

An unexpected hardware invalidation timeout marks the remapping unit failed
and is fatal in required mode. At runtime this is a kernel panic, not
merely a failed boot — deliberately harsher than a wedged *device*, which
stays contained (§0): continuing with uncertain invalidation state would
make prior translations an unbounded memory authority.

## 8. Ublock and process-lifetime integration

Add DMA-pin state to `struct ublock`. The exact representation may be a
count or an intrusive list of mapping references; v1 needs at least a count
plus the domain mapping records that identify who owns each pin.

Rules while `dma_pins != 0`:

- `SYS_VM_FREE` returns `SYSERR_EXIST`.
- `SYS_VM_MOVE` returns `SYSERR_EXIST`; identity IOVA must not silently
  change security ownership.
- `SYS_VM_PROTECT` remains a CPU-view operation and is allowed. Removing CPU
  write access does not revoke a device-write grant.
- `SYS_VM_SHARE` may still create CPU views, but it grants no DMA authority.
- driver death leaves the block on the zombie and keeps all pins/mappings;
  there is no use-after-free window.
- process reap handles IOMMU state before ordinary ublocks: it detaches one
  context or unmaps one whole block per step and reports `REAP_MORE` until
  every DMA pin is gone.

The intended tree shape is `init -> pcid -> hardware drivers`. `pcid` gets a
tree event when a driver dies, resets/disables its devices and clears bus
mastering, then drives that child's reap loop. Kernel reap owns IOMMU object
cleanup; `pcid` owns the device-specific action that stops useful DMA and
prevents a fault storm.

A CPU share to the driver is not implicit consent for device DMA. V1 maps
only blocks the driver itself owns. Later zero-copy needs an explicit
owner-created DMA grant whose revoke protocol waits for device quiescence;
ordinary `VM_SHARE` must not acquire that meaning retroactively.

## 9. Fault delivery

The architecture backend normalizes at least these reasons:

```c
#define IOMMU_FAULT_CONTEXT_MISSING  1
#define IOMMU_FAULT_PTE_MISSING      2
#define IOMMU_FAULT_READ_DENIED      3
#define IOMMU_FAULT_WRITE_DENIED     4
#define IOMMU_FAULT_ADDRESS_WIDTH    5
#define IOMMU_FAULT_INTERNAL         6
```

Per attached device the core keeps a fault counter and at most one
unconsumed `KEV_IOMMU_FAULT`. Additional faults coalesce while that event is
outstanding. After the driver advances `cq_head` and rings its consumption
ack, replay posts the latest record if the counter moved. Thus a broken
device cannot overflow the CQ or force unbounded interrupt work.

`KIOMMU_DEVICE_ATTACH` enforces event capacity with the same accounting as
IRQ claims: enough CQ space for one outstanding event per device plus
command completions. The event cookie is chosen by the driver; the kernel
retains the real requester ID internally and
includes it in serial diagnostics.

The initial backend may poll fault registers during tests. Before drivers
rely on fault recovery, delivery must use the hardware fault interrupt or
another bounded mechanism that cannot silently lose a full fault log.
Interrupt-driven delivery reuses the IRQ design's route-lock machinery —
shared code, not a parallel implementation. `DEVICE_ATTACH` installs a
fault route whose lock pins the device-to-ring association for the
data-plane post path; the handler records and clears a bounded number of
hardware records under the unit-local lock and posts or coalesces the
event through that route, never taking the global umem lock; and
`DEVICE_DETACH` or reap severs the route under the route lock before the
device record or ring block can be freed. Any required
control-plane cleanup is initiated later by the driver or its reaper.

## 10. Intel VT-d backend

The VT-d implementation lives under `kernel/archsrc/x86_64/` and follows
the Intel VT-d architecture specification. V1 supports legacy translation
mode:

```
root[bus] -> context[devfn] -> {domain id, second-level root}
                                  |
                                  `-> 4-level IOVA page tables
```

### DMAR subset

Parse and validate:

- the DMAR header and host-address width;
- DRHD entries and register bases;
- PCI segment numbers;
- include-all units;
- PCI endpoint and bridge device scopes needed to resolve coverage;
- RMRR entries.

The first QEMU milestone may require exactly one segment-zero include-all
DRHD and no RMRR. Encountering unsupported scopes is a boot error in required
mode, not permission to bypass translation. Real-hardware work then expands
coverage deliberately.

RMRRs are never placed in every domain. When support lands, the kernel maps
an RMRR only into the domain of a firmware-named device and pins/reserves the
range outside the buddy. A device with an unsupported required RMRR cannot
be attached.

### Hardware bring-up

- Map the remapping register range kernel-only `PAGE_R|PAGE_W|PAGE_UC`
  before cloning user page tables.
- Check CAP/ECAP for the selected address width, second-level translation,
  4 KiB pages, 2 MiB superpage support (SLLPS; fall back to 4 KiB leaves
  if absent), and supported invalidation path.
- Allocate a 4 KiB root table and context tables on demand.
- Program RTADDR, issue Set Root Table Pointer, and wait for status.
- Perform global context-cache and IOTLB invalidation.
- Set Translation Enable and wait for status.
- Configure fault recording; initially route its interrupt to the BSP.

Table stores use release ordering before invalidation. Context-cache and
IOTLB invalidations are synchronous from the caller's point of view and
have fixed iteration/time bounds. Runtime register-based invalidation is
adequate for v1; queued invalidation is a performance follow-up.

Domain IDs are kernel allocated and are not driver cookies. They are not
reused until every context using the old ID is absent and the required
domain/global invalidations have completed.

ATS is disabled/not enabled on assigned PCI functions in v1. Allowing a
device-side translation cache introduces its own invalidation and teardown
protocol and is incompatible with the simple domain guarantee above.

## 11. Future architecture backends

### AMD-Vi

AMD-Vi consumes ACPI IVRS, uses its own device table and command/event
mechanisms, and implements the same common domain operations. It is a
separate backend, not conditionals spread through the VT-d code.

### Arm SMMUv3

SMMUv3 consumes ACPI IORT on standards-based systems or a platform Device
Tree on embedded targets. PCI requester IDs and integrated-device IDs map
to StreamIDs. A stream-table entry selects a stage-2-only translation
context for the v1 govindos model; command and event queues implement
invalidation and faults.

The common domain and ublock API stays unchanged. The backend translates
`iommu_device_id` through firmware-described ID mappings, and identity IOVA
remains valid when the platform physical-address geometry permits it.

## 12. Interaction with PCI, MMIO, IRQs, and NVMe

- **PCI config:** `pcid` alone owns ECAM and the PCI command register. No
  driver receives config-space frames or CF8/CFC port access. V1 uses the
  devmem grants described by the PCI and capability designs.
- **BARs:** BAR ranges arrive as device-backed ublocks created by
  `SYS_VM_MAP_DEVICE` and shared in by `pcid`. They are flagged
  never-DMA-mappable, so `KIOMMU_MAP_BLOCK` rejects them.
- **MSI/MSI-X:** `pcid` obtains an opaque kernel-selected pair plus a route
  token and programs the device; the IRQ scheme verifies the token when the
  driver binds. VT-d interrupt remapping later changes
  only how the opaque pair is composed.
- **NVMe queues:** submission queues, completion queues, PRP-list pages, and
  bounce buffers are ordinary driver-owned ublocks mapped RW into the NVMe
  domain before controller enablement.
- **NVMe client I/O:** v1 copies between the client channel arena and an
  already mapped bounce buffer. A client can revoke its channel without a
  device retaining a translation to recycled client memory.
- **Cache attributes:** DMA buffers are normal coherent WB RAM. Device BARs
  and remapping-unit registers are UC CPU mappings. An MMIO range is never
  installed in an IOMMU RAM domain.

The IOMMU protects system memory even if an untrusted NVMe driver writes a
malicious PRP. Withheld ECAM authority prevents it from moving BARs, enabling
ATS, or changing requester-visible PCI features. Keeping MSI programming in
`pcid` denies it configuration-space control of MSI, but does not prevent
interrupt spoofing: a DMA write to the `0xFEE` interrupt address range
bypasses DMA remapping (§0), so drivers are memory-isolated, not
interrupt-isolated, until interrupt remapping lands.
It can still corrupt its own mapped buffers, wedge its device,
destroy on-disk data it was authorized to access, and consume service time.

## 13. Locking

There is no global IOMMU lock. Every control-plane path already serializes
on `g_umem`: driver commands arrive through scheme drains, which run
holding it; process reap enters with it; boot bring-up is single-threaded.
A nested `g_iommu` would never be acquired independently and would protect
nothing. The control-plane state — domain-ID allocator,
requester-to-domain assignments, domain and mapping records — is therefore
`g_umem`-protected like the rest of the ownership graph.

Order for control commands:

```
g_umem -> process ulock -> domain/unit leaf locks
```

The synchronous invalidation waits held under `g_umem` are
microsecond-scale register polls that occur only at configuration,
teardown, and reap time; steady-state I/O issues no IOMMU commands. If that
stops being true — ATS device-TLB or queued invalidation is adopted, or
measured real-hardware invalidation latency stalls the control plane —
introduce a `g_iommu` together with the `as_flush_multi` pin-and-drop
pattern: pin the affected objects, drop `g_umem`, perform the wait under
`g_iommu`, reacquire. Introducing the lock without dropping `g_umem`
across the wait buys nothing.
Hardware fault handlers use a unit-local leaf lock, the shared IRQ
fault-route lock, and the existing stripe-ranked CQ post path only; they
never take `g_umem`. The route lock is the lifetime pin:
detach and reap sever the fault route under it — following the same
rank/side rules as IRQ routes — before the device record or ring block is
freed.
Driver commands or process reap later reconcile the recorded fault on the
control plane.

IOMMU reap extends the existing process-reap state machine. Entered with
`g_umem`, it performs one detach or one whole-block
unmap per step, including the required synchronous invalidation, then returns
`REAP_MORE`. It does not pretend to submit commands through the dead ring.
Only after every IOMMU object and pin is gone may ordinary block reap reach
the control ring or mapped ublocks.

## 14. Security invariants

The implementation is correct only while all of these hold:

1. No govindos-controlled PCI function has Bus Master Enable set before it
   has a present context in a live domain.
2. Every attached requester is covered by exactly one initialized unit and
   belongs to at most one domain.
3. Every present IOMMU leaf points inside the physical extent of a currently
   pinned ublock owned by that domain's driver, except explicit future RMRR
   mappings.
4. A pin is acquired before publishing the first leaf and released only
   after removing the last leaf and completing IOTLB invalidation.
5. A context is published only after its page-table root is initialized; a
   context is considered detached only after context-cache and IOTLB
   invalidation.
6. No user mapping includes IOMMU registers or remapping-table pages.
7. Unsupported firmware topology or hardware capability fails closed.
8. A fault, driver death, or hardware timeout never installs a passthrough
   mapping as recovery.

## 15. Verification plan

### Table and parser tests

- Valid and truncated DMAR tables; checksum and length failures.
- Overlapping/include-all DRHD coverage.
- Invalid device scopes and unsupported segments.
- Page-table map/unmap at level boundaries and address-width limits.
- Superpage and 4 KiB fallback map paths; blocks over the leaf cap are
  rejected without hardware-visible state.
- Permission changes and attempted conflicting mappings.
- Domain-ID and driver-cookie reuse.

### QEMU hardware tests

1. Boot Q35 with `intel-iommu`; observe default-deny translation enabled.
2. Boot without `intel-iommu`; required-mode boot must fail loudly before
   init.
3. Attach a QEMU NVMe function to an empty domain.
4. DMA to a mapped queue/buffer succeeds.
5. A PRP pointing at kernel memory faults and leaves the target unchanged.
6. A PRP pointing at an owned but unmapped driver block faults.
7. Device write to a device-read-only mapping faults.
8. After unmap and invalidation, the former IOVA faults.
9. Two domains cannot DMA into each other's blocks.
10. Kill the driver with I/O outstanding: blocks remain pinned; `pcid`
    resets the device, then bounded kernel reap detaches/unmaps and only
    afterward returns the blocks to the buddy.
11. Repeated domain construction/destruction returns kernel table counts,
    mapping counts, pins, and domain IDs to their baselines.

Tests should inspect normalized fault events and the VT-d fault record, not
merely rely on an NVMe timeout. The kernel-memory-DMA tests are the acceptance
criterion for calling the driver isolated.

## 16. Suggested implementation order

1. Generalize ACPI lookup from MADT-only to checked four-character table
   lookup; add DMAR structures and parser tests.
2. Add the architecture-neutral IOMMU core, required-mode boot result, fixed
   resource bounds, and backend interface.
3. Add VT-d register mapping/capability reporting and QEMU Q35 harness.
4. Build legacy root/context and second-level tables with unit tests.
5. Enable translation default-deny and provoke a controlled missing-context
   fault.
6. Implement domain/device attach and detach with synchronous invalidation;
   keep authorization behind the single replaceable hook in §5.
7. Add ublock DMA pins and single-invocation whole-block map/unmap reusing
   the paging.c walk (4 KiB and 2 MiB superpage leaves).
8. Add the per-process direct-driver ring ABI and automatic IOMMU reap
   steps.
9. Add fault interrupt delivery and coalesced `KEV_IOMMU_FAULT` events,
   reusing the IRQ route-lock delivery machinery as shared code.
10. Implement the driver/`pcid` lifecycle: the driver configures its domain,
    while `pcid` retains ECAM, enables bus mastering last, and disables it
    first on teardown.
11. Put the NVMe admin/I/O queues and bounce pool in the domain; run the
    kernel-memory-PRP acceptance tests.
12. Expand real-hardware DMAR scopes/RMRR coverage.
13. Replace the §5 attach hook and PCI/IRQ grant stubs with pure capabilities
    when the general capability model lands.
14. Add VT-d interrupt remapping behind `KIRQ_MSI`.
15. Add AMD-Vi and SMMUv3 backends, then optional/trusted fallback modes if
    they are still desired.

Steps 1-7 are the minimum kernel DMA-isolation milestone. Steps 8-11 turn it
into a usable direct userspace-driver interface. Optional IOMMU modes must
not land until the required path and its failure behavior are routine; the
secure path is the architecture, not a special case layered on a permissive
default.
