# Capability design: cryptographic capabilities, partitioned exercise

Status: **planned 2026-07-14; revised same day — sparse blobs + `SYS_CAP_ADOPT`
are the universal transfer mechanism, and flat trust domains replace the
earlier per-process adopt-deny bit as the containment failsafe.**
Companion to [ipc-process-design.md](ipc-process-design.md),
[irq-design.md](irq-design.md), [iommu-design.md](iommu-design.md), and
[pci-design.md](pci-design.md). This is the "general capability model" those
documents defer to: it replaces the four `TODO(capability)` authorization
stubs (`umem.c` VM_MAP_DEVICE no-policy, `irq.c` direct-child provenance ×2,
`iommu.c` first-claim attach) and the "userspace is trusted in v1" caveats
in the PCI/IOMMU docs.

## Why sparse-first

Most authority in a grown system is **userspace authority**: filesystem
rights, port/network rights, service-object rights. Two govindos facts
decide the mechanism for those:

1. The kernel cannot and should not model server semantics. A kernel cap
   type per server concept is unbounded work and unportable policy.
2. Govindos IPC makes kernel-mediated per-object handles uneconomical.
   On seL4 or Fuchsia, "cap to a file" is cheap because an open file is a
   kernel channel/endpoint and handles ride kernel-copied messages. Here a
   connection is a whole shared block with two waiter slots, and no
   message ever passes through the kernel. The natural shape is one ring
   per client↔server pair with **many objects multiplexed over it** — so
   object authority must be expressible as *data in the ring*. That is a
   cryptographic token by definition.

So the model is **one idiom, two verifiers**. All authority travels as
opaque blobs in rings:

- **Userspace objects** (files, ports, service objects): tokens issued and
  verified by the owning server under its own secret (§8). The kernel
  never sees them; zero kernel design or porting surface.
- **Kernel objects** (device memory, IRQ routes, IOMMU attach): the same
  transport, but a blob is inert until `SYS_CAP_ADOPT` mints it into the
  caller's cap table, and **all exercise takes table handles**. Sparse
  transport, partitioned exercise.

The exercise layer stays partitioned because govindos teardown — revoke on
death, bounded reap, "liveness, revocation, and death can never disagree" —
is an *enumeration* property, and pure verify-at-use sparse caps are
anti-enumeration: the kernel could no longer answer "what can this process
do", and revocation degenerates into blacklists or epoch bumps. ADOPT
costs one syscall per grant and buys those invariants back for exactly the
resources where they are load-bearing (DMA, interrupts, MMIO).

Containment is honest about its limits: even perfect confinement cannot
stop a cap holder from *proxying* for a peer that owns it (a buffer
overflow makes any process a puppet), so leakage-resistance is a matter of
degree, not kind. The first wall is trusted processes not leaking blobs.
The second wall — the failsafe — is the **trust domain** (§3): a leaked
kernel-cap blob is unadoptable across a jail boundary.

## 0. Decisions log

- **No untyped/retype, no CSpace.** The kernel keeps its allocator and
  per-process accounting; the cap table is flat, kernel-allocated, charged
  to its owner. This buys authority hygiene, not seL4-class verification,
  and the document says so plainly.
- **A handle is a name, not a secret**; rights live in the slot, never in
  the handle value; generation stamps make stale handles fail loudly
  (the `irq_route` pattern).
- **Rights and parameters are diminish-only**, via `SYS_CAP_MINT`
  (attenuate-then-export is the delegation idiom).
- **Blobs are claims on live slots, not freestanding authority.** A blob
  references the exporting slot `{pid, index, generation}` + MAC; adoption
  requires that slot to still exist and links the adopted cap as its
  derivation child. Zero kernel state per blob; revoking or dropping the
  exporting slot (or the exporter dying) kills every outstanding blob
  automatically. Multi-adopt is allowed — each adoption is a new
  enumerable child (one-shot blobs would need a kernel nonce table).
- **Per-boot MAC key.** Blobs are transport, never persistence. MAC
  compare is constant-time; tag ≥ 128 bits.
- **Pids are never reused** (monotonic 64-bit allocation). Blob safety
  requires it; the phase-2 identity split (§10) wants it anyway.
- **Trust domains are the containment failsafe**: a flat immutable label,
  equality-checked at the single ADOPT choke point. Chosen over the
  earlier `PROC_NO_ADOPT` bit because it composes with unmodified
  programs — inside a jail the full cap machinery works; only the
  boundary is a wall.
- **Embryo `CAP_MOVE` is the one non-blob transfer** and the one
  sanctioned domain crossing: parent-driven construction places birth
  authority directly, pre-seal.
- **The hybrid boundary is a bright line.** Everything reachable through a
  scheme SQE takes cap handles; the core VM/proc ABI keeps tree rules
  until phase 2 (§10). This coincides with the existing rule "schemes
  exist only where the kernel delivers events back".
- **Cap checks add to, never replace, kernel safety checks.** A CAP_DEVMEM
  grant does not exempt `umem_map_device` from validating that the range
  is genuinely non-RAM device memory.

## 1. Kernel objects

### 1.1 Handle

```
handle (u64):
  [63:32] generation — copied from the slot at mint; 0 never used
  [31:0]  slot index — dense index into the holder's cap table
  value 0 = CAP_NULL
```

Validation at a use site is one bounded lookup under a lock the operation
already holds: index in range → slot type matches the op → generation
matches → rights cover the op. Bad handle or wrong type/generation is
`SYSERR_INVAL`; insufficient rights is `SYSERR_PERM`. Handles resolve only
in the caller's own table; leaking a handle value grants nothing.

### 1.2 Slot

```c
// One slot in a process's cap table (kernel-side, table charged to the
// owner's account). generation bumps on every free, skipping 0, so a
// stale handle or blob can never alias the slot's next tenant.
struct cap {
  uint8_t  type;       // CAP_NULL / CAP_DEVMEM / CAP_IRQ_ROUTE / CAP_IOMMU_DEV
  uint32_t rights;     // CAPR_MINT | CAPR_EXPORT | per-type bits; diminish-only
  uint32_t generation;
  union {              // per-type object reference / parameters
    struct { uint64_t base, len; uint32_t flags; } devmem;
    struct { struct irq_route *route; } irq;
    struct { uint16_t rid; } iommu_dev;   // requester id (group variant later)
  };
  // Derivation tree, intrusive. dparent's slot may belong to another
  // process (adoption links across tables). Walked by bounded revoke;
  // a slot with children cannot DROP.
  struct cap *dparent, *dchildren, *dsibling;
  struct process *holder;
};
```

Tables, slots, and derivation links live under `g_umem` (the control plane
already guarding the pid registry, share edges, and ring state), so cap
validity can never disagree with liveness or revocation. Per-route /
per-object delivery state keeps its own locks; the cap layer only decides
whether an operation may begin.

`CAPR_EXPORT` gates both `SYS_CAP_EXPORT` and embryo `SYS_CAP_MOVE`.
Per-type rights reuse existing flag vocabularies where one exists
(CAP_DEVMEM rights are the `VM_DEVICE_*` bits).

### 1.3 Types (v1)

- **CAP_DEVMEM {base, len, flags}** — authority to `SYS_VM_MAP_DEVICE`
  any page-aligned subrange with any subset of `flags`. Root cap covers
  all non-RAM platform space, all flag bits; `pcid` mints narrowed
  BAR-range children for drivers (pci-design's "future BAR-range
  capability"; ECAM authority is a devmem cap over the ECAM window).
- **CAP_IRQ_ROUTE {route}** — authority over one `irq_route`: claim,
  bind, release. Root is a route-wildcard parent with MINT held by init;
  per-GSI children are pure mints. MSI routes allocate hardware state, so
  they materialize via the `KIRQ_MSI` scheme op (presenting a parent
  cap), not a pure mint.
- **CAP_IOMMU_DEV {rid}** — authority to attach/detach that requester id
  to a caller-owned domain. Root is rid-wildcard with MINT, delegated
  init → `pcid`, which owns topology discovery and mints per-device
  children. Replaces first-claim-wins; realizes iommu-design §5's
  `CAP_IOMMU_SPACE` placeholder (per-rid first; group caps after ACS).

## 2. Blobs

```c
#define CAP_BLOB_SIZE 32
struct cap_blob {          // opaque to userspace; layout is kernel-private
  uint64_t pid;            // exporting process (never reused)
  uint32_t index, generation; // exporting slot
  uint8_t  mac[16];        // MAC_k over the above; k is per-boot
};
```

A blob is *transport*: it confers nothing until adopted, and it carries no
authority state — validity is entirely "does the referenced slot still
exist". Consequences, all free of new kernel state:

- Exporter drops the slot, has it revoked, or dies → every outstanding
  blob from it is dead (`SYSERR_DEAD` at adopt).
- Attenuation before export: `CAP_MINT` a narrowed child, export that;
  revoking the narrowed slot kills exactly that delegation chain.
- A blob may be adopted multiple times; each adoption is a derivation
  child of the exporting slot — enumerable and revocable as a subtree.
- Blobs never survive reboot (per-boot key); persistence is a userspace
  concern (§8).

## 3. Trust domains

A trust domain is a flat `uint64_t` label on every process: inherited from
the parent at `PROC_CREATE`, immutable for life, never reused. Init is
domain 0. `SYS_PROC_CREATE(PROC_NEW_DOMAIN)` starts the embryo in a fresh
domain — that is the entire lifecycle: no kernel object, no cap type,
nothing to reap, and jails nest for free.

One rule, at one choke point:

> `SYS_CAP_ADOPT` requires the adopter's domain to equal the exporting
> slot holder's domain.

Domains are immutable, so the check races nothing. The blob format is
unchanged — the domain is derived from the live exporting slot, not
carried in the blob.

What this buys: within a domain (the common case — the base system is one
domain), blobs flow freely over rings with no ceremony; a compromised or
buggy trusted process that leaks a blob *inside* the domain has leaked to
processes that could already have proxied through it — degree, not kind.
Across a jail boundary, a leaked kernel-cap blob is **inert**: the second
wall holds even when the first fails. Any unmodified program can run
jailed; nothing needs to be designed for confinement.

Crossings:

- **Embryo `CAP_MOVE`** (§4) crosses domains by design: the parent
  constructing a jailed child places its birth authority directly,
  pre-seal. This is the deliberate, parent-driven act that defines the
  jail's kernel authority.
- **Accepted limitation:** granting *new* kernel authority to an
  already-running jailed process is impossible except by an outside proxy
  operating on its behalf. That is the failsafe working as intended. If a
  real need appears, a gated cross-domain export can be added without
  blob-format changes.
- Userspace tokens (§8) are not domain-checked by the kernel — servers
  may adopt the same discipline in their verifiers if they care (a
  caveat naming the client, §8), but that is server policy.

## 4. Syscall surface

New syscalls (numbers continue from `SYS_VM_MAP_DEVICE 17`):

```c
#define SYS_CAP_MINT   18 // (handle, rights, p0, p1) -> new handle
#define SYS_CAP_EXPORT 19 // (handle, buf)            -> 0; writes CAP_BLOB_SIZE bytes
#define SYS_CAP_ADOPT  20 // (buf)                    -> handle
#define SYS_CAP_REVOKE 21 // (handle)                 -> REAP_* | SYSERR_AGAIN
#define SYS_CAP_DROP   22 // (handle)                 -> 0 | SYSERR_EXIST
#define SYS_CAP_MOVE   23 // (handle, pid)            -> handle in embryo's table
#define SYS_MAX        24
```

- **SYS_CAP_MINT (handle, rights, p0, p1)** — derive a child cap. Needs
  `CAPR_MINT`. `rights` ⊆ parent's; `p0/p1` narrow type parameters
  (devmem: sub-base/len; irq: GSI; iommu: rid) within the parent's;
  0/0 = same as parent. Links under the parent; charged to the caller.
  Bounded: one slot, one link.
- **SYS_CAP_EXPORT (handle, buf)** — write the blob for this slot to a
  user buffer. Needs `CAPR_EXPORT`. The slot itself is untouched; export
  any number of times. The caller then sends the bytes over any ring it
  likes — the kernel is not in that path.
- **SYS_CAP_ADOPT (buf)** — verify the blob (constant-time), require the
  exporting slot live (`SYSERR_DEAD` otherwise) and same trust domain
  (`SYSERR_PERM`), then mint a slot in the caller's table with the
  exported slot's type/rights/params, linked as its derivation child.
  Returns the new handle. Malformed/bad-MAC blobs are `SYSERR_INVAL`.
- **SYS_CAP_REVOKE (handle)** — destroy the cap's *derived subtree* (the
  cap survives; drop yourself with DROP). One bounded step per call:
  unlink and free one deepest-first descendant slot, wherever it lives;
  `REAP_MORE` until `REAP_DONE`. Objects with live claims (a revoked
  route cap whose route is claimed to a ring) are torn down in the same
  step via the object's existing bounded teardown hook. Outstanding
  blobs from revoked slots die implicitly (§2).
- **SYS_CAP_DROP (handle)** — free the slot. `SYSERR_EXIST` while
  derivation children exist (revoke first), mirroring `VM_FREE` on a
  scheme block. Never cascades.
- **SYS_CAP_MOVE (handle, pid)** — transfer the slot (not a copy) into an
  **own embryo's** table, pre-seal only; needs `CAPR_EXPORT`. The one
  non-blob transfer and the one sanctioned domain crossing. Returns the
  embryo-side handle value, which the parent passes via the image /
  `THREAD_SPAWN` arg convention. Derivation links are preserved.

### 4.1 Changed syscalls

- **SYS_PROC_CREATE (flags)** — gains its first argument.
  `PROC_NEW_DOMAIN (1u)`: the embryo starts a fresh trust domain (§3).
- **SYS_VM_MAP_DEVICE (cap, base, len, flags)** — gains a leading
  CAP_DEVMEM handle (4 args, within the register convention). Requested
  range ⊆ cap range, `flags` ⊆ cap flags ∩ rights; existing kernel range
  validation (non-RAM, alignment, FIRMWARE only on ACPI backing)
  unchanged on top.

Everything else in the core ABI (`VM_*`, `BLOCK_*`, `PROC_KILL/REAP`,
`THREAD_SPAWN`, `GETPID`) is untouched in phase 1.

### 4.2 Changed scheme ops

IRQ ring (`kring_irq.h`), replacing both provenance stubs:

```c
#define KIRQ_CLAIM   1 // a = CAP_IRQ_ROUTE handle (was: bare GSI), b = cookie
#define KIRQ_RELEASE 2 // a = CAP_IRQ_ROUTE handle
#define KIRQ_ACK     3 // a = GSI/pseudo-gsi, b = seq (UNCHANGED, see below)
#define KIRQ_MSI     4 // a = parent CAP_IRQ_ROUTE handle (was: child pid)
                       // completion: a = MSI addr/data pack, b = new route cap handle
#define KIRQ_BIND    5 // a = CAP_IRQ_ROUTE handle (was: granted route id), b = cookie
```

`KIRQ_ACK` stays handle-free deliberately: acking is scoped to a route
already claimed/bound *to this ring*, so the ring is the authority context
and the hot path stays a raw indexed op. Authority is spent at claim/bind,
not per interrupt. `KIRQ_MSI` no longer names a target process: pcid
allocates against its own parent cap, receives the route cap in the
completion, programs MSI address/data into the device (its job per
irq-design), and exports the route cap to the driver over their ring;
the driver adopts and `KIRQ_BIND`s it. The "direct child" restriction
disappears — pcid and drivers can be siblings.

IOMMU ring (`kring_iommu.h`):

```c
#define KIOMMU_DEVICE_ATTACH 3 // requester field = CAP_IOMMU_DEV handle (was: bare rid)
#define KIOMMU_DEVICE_DETACH 4 // same
```

`DOMAIN_CREATE/DESTROY` and `MAP_BLOCK/UNMAP_BLOCK` are unchanged: domains
are implicitly self-owned and mapping is already restricted to the
caller's own ublocks — possession-based by construction. This is the
"SQE's requester field becomes a capability handle" swap iommu-design §5
reserved space for.

Scheme *creation* policy (`channel_scheme_create`) stays open: an IRQ or
IOMMU ring whose owner holds no caps is inert, so gating creation adds
nothing (resolves the init.c selftest note "creation policy moves to
capabilities later").

## 5. Death, reap, and revocation

A cap is reachable from two directions and must die from both:

1. **Holder dies.** The zombie's cap table is torn down by new
   `process_reap_step` step types: "revoke one derivation edge under a
   held cap", then "drop one held cap", deepest-first, one per step.
   Caps are reaped before owned blocks (a route cap's teardown may need
   to release a claim on a ring living in an owned block — verify this
   ordering against `channel_block_destroyable` sequencing at
   implementation time).
2. **Granter dies.** Caps derived from the dead process's slots — adopted
   copies held by live processes anywhere — are part of the derivation
   subtrees revoked in (1). Delegated authority dies with the delegator;
   the holders survive, minus the grant. Composes with recursive kill in
   reap's own bounded budget.

Blobs add nothing to any of this: they are stateless claims that fail at
adopt time once their slot is gone. There is no object→cap reverse index
in v1: the only objects caps reference (routes, rids, devmem ranges) are
static platform resources that never die. Cap *targets* that die arrive
with phase-2 CAP_PROC (§10).

## 6. Bootstrap

The kernel mints the root set into init's table (domain 0) before sealing
it, and describes it in a new bootinfo section (array of
`{handle, type, rights, p0, p1}`):

- one CAP_DEVMEM, all non-RAM space, all flags, MINT|EXPORT;
- one CAP_IRQ_ROUTE parent (route-wildcard), MINT|EXPORT;
- one CAP_IOMMU_DEV parent (rid-wildcard), MINT|EXPORT.

Init delegates by attenuate-then-export over its rings (or embryo
CAP_MOVE at spawn): devmem and iommu parents to `pcid`; route caps flow
pcid → drivers as devices are claimed. The kernel never expresses policy
beyond "possession of a sufficient cap" — who deserves what is entirely
init's and pcid's construction, which is the microkernel answer to
sandboxing: confinement is the connectivity-and-domain graph, not a
kernel policy engine. Boot selftests get a kernel-side mint hook (they
already run pre-init with kernel-driven process creation).

## 7. Conversion table

| Site | Today | After |
|---|---|---|
| `umem_map_device` (`umem.c` "no caller policy") | any process maps any validated range | CAP_DEVMEM covering range+flags |
| `allocate_msi` (`irq.c` "direct-child provenance") | `target->parent == curr->proc` | parent CAP_IRQ_ROUTE presented in SQE |
| `bind_msi` (`irq.c` "target provenance") | `route->target == owner` | CAP_IRQ_ROUTE possession; `route->target` deleted |
| GSI `claim` (`irq.c`) | open first-claim by GSI number | CAP_IRQ_ROUTE for that GSI |
| `DEVICE_ATTACH` (`iommu.c` "bare first-claim") | first well-formed unclaimed rid wins | CAP_IOMMU_DEV for that rid |
| pcid BAR share (pci-design "temporary share") | pcid VM_SHAREs its BAR block | pcid mints+exports sub-range CAP_DEVMEM; driver adopts and maps its own block |

## 8. Userspace tokens

The larger half of the design is deliberately not kernel code. Servers
(a future bootfs/fsd, netd, pcid's topology queries) issue tokens for
their own objects under their own secrets, verify them on their own
rings, and define their own revocation. Recommended shape, provided as a
`gdoslib` helper (`token.c`) so servers share one audited implementation:

- **Issue:** `t0 = {object, rights, epoch} ‖ HMAC(k_server, ·)`.
- **Attenuate offline, macaroon-style:** `t1 = t0 ‖ caveat ‖
  HMAC(t0.mac, caveat)` — any holder can narrow (path prefix, read-only,
  expiry, max-bytes) without contacting the server; verification replays
  the chain under `k_server`.
- **Bind to a peer (optional):** a caveat naming the presenting client
  (e.g. the pid on the far side of the ring, which the server knows from
  the share edge) turns a bearer token into a possession-bound one where
  it matters.
- **Revoke:** per-object epoch bump (coarse, stateless) or a server-side
  denylist of issuance ids (fine, server's state, server's problem).

Stated plainly: userspace authority is **bearer authority** by design —
a leaked file token is usable by anyone with a ring to that server,
until epoch/expiry. That is the accepted trade for zero kernel
involvement, offline attenuation, and portability of server semantics;
servers that need more bind to peers or expire aggressively. Kernel
objects get the stronger ADOPT story because DMA and interrupts are
where a durable leak is unrecoverable.

Tokens are opaque bytes to the kernel; nothing here appears in the ABI
headers except nothing at all.

## 9. Invariants

- Kernel authority is enumerable at exercise: "what kernel objects can
  process P operate" is P's cap table, answerable under `g_umem`. Blobs
  in flight are claims, not authority, and add zero kernel state.
- Handles are meaningless outside their table; blobs are inert outside
  the exporter's trust domain and dead after the exporting slot dies.
- Rights and parameters only ever shrink along derivation edges (mint,
  adopt, embryo move all preserve or diminish).
- A jail's kernel authority is bounded by birth placement plus what
  same-domain peers export to it; a fresh-domain jail with no placement
  can never acquire any, regardless of what leaks into its rings.
- Every cap operation is bounded; every unbounded teardown is a
  caller-driven `REAP_MORE` loop.
- No cap check replaces a safety check.

## 10. Deferred

- **CAP_PROC (phase 2).** Replace pid-as-authority Fuchsia-style: keep
  the never-reused numeric id as pure identity (events, logs, `GETPID`),
  move KILL/REAP/SPAWN/MOVE_IN/PROTECT to CAP_PROC handles with rights
  bits; `PROC_CREATE` returns handle + id; the embryo window becomes a
  rights diminishment at seal. Costs that make it phase 2: every proc
  syscall changes shape; kernel-posted events (`KEV_CHILD_DEAD`) need
  mint-time cookies; processes are the first cap targets that *die*,
  requiring per-process lists of caps referencing them, walked by reap.
  Reap authority stays parent-only regardless — reap decides where
  salvaged blocks go (tree edges), not just who may trigger it.
- **Cross-domain export**, gated on a future domain-referencing cap, if
  granting new kernel authority into a running jail ever becomes a real
  need (§3).
- **Group caps / ACS.** CAP_IOMMU_DEV is per-rid until pci-design's ACS
  work defines isolation groups.
- **Persistence.** The kernel key is per-boot; durable authority is a
  userspace concern (servers re-issue tokens; init re-delegates from the
  root set).

## 11. Implementation order

1. Cap table + slot/generation/derivation core under `g_umem`; MINT and
   DROP; accounting; selftests.
2. Bounded REVOKE and the reap step types (holder-death and granter-death
   paths); kill/reap selftests with cross-process derivation.
3. Root cap minting + bootinfo section; init plumbing; domain label on
   `struct process` + `PROC_CREATE(flags)`/`PROC_NEW_DOMAIN`.
4. Per-boot key, EXPORT/ADOPT with domain check; blob selftests
   (stale slot, dead exporter, cross-domain, bad MAC).
5. Embryo CAP_MOVE.
6. Convert `SYS_VM_MAP_DEVICE` (CAP_DEVMEM); delete the umem.c TODO;
   pcid mints driver sub-ranges.
7. Convert the IRQ scheme ops (CLAIM/MSI/BIND/RELEASE by handle); delete
   both irq.c TODOs and `route->target`.
8. Convert `KIOMMU_DEVICE_ATTACH/DETACH`; delete the iommu.c first-claim
   hook; update nvmed + QEMU harness.
9. `gdoslib` token.c (issue/attenuate/verify) + first server adoption.
10. Docs pass: retire the "trusted userspace v1" caveats in
    pci-design/iommu-design; note divergences here.
