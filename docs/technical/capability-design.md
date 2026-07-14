# Capability design: grants, macaroon tokens, and the cap channel

Status: **planned 2026-07-14, revised twice same day. Final shape: kernel
state is a small tree of revocation anchors ("grants"); authority is
HMAC-chained tokens attenuated and transferred entirely in userspace;
grant management is a kernel scheme (`KSCHEME_CAP = -2`), not syscalls.**
Companion to [ipc-process-design.md](ipc-process-design.md),
[irq-design.md](irq-design.md), [iommu-design.md](iommu-design.md), and
[pci-design.md](pci-design.md). This is the "general capability model"
those documents defer to: it replaces the four `TODO(capability)`
authorization stubs (`umem.c` VM_MAP_DEVICE no-policy, `irq.c`
direct-child provenance ×2, `iommu.c` first-claim attach) and the
"userspace is trusted in v1" caveats in the PCI/IOMMU docs.

## Why this shape

Most authority in a grown system is **userspace authority** — filesystem
rights, port rights, service-object rights — and two govindos facts decide
the mechanism:

1. The kernel cannot and should not model server semantics; a kernel cap
   type per server concept is unbounded and unportable.
2. Govindos IPC makes kernel-mediated per-object handles uneconomical. A
   connection is a whole shared block; no message ever passes through the
   kernel. The natural shape is one ring per client↔server pair with many
   objects multiplexed over it — so object authority must be expressible
   as *data in the ring*. That is a cryptographic token by definition.

So there is **one idiom**: authority is token bytes, attenuated offline
macaroon-style, copied over rings with no kernel involvement. Userspace
servers verify tokens for their own objects under their own secrets (§8).
The kernel verifies tokens for kernel objects under the boot key — it is
just another server that happens to enforce at the hardware. Because even
grant *management* happens over a channel (§5), the kernel's authority
service is **substitutable**: a client cannot tell whether its authority
ring is serviced by the kernel or by an interposing supervisor (§6).

Kernel state exists only where revocation (or a trust-domain crossing)
is wanted. That is the deliberate cost model: *revocability is what
kernel state is for*. Everything between revocation points is bytes.

Containment is honest about its limits: no design stops a token holder
from proxying for a peer that owns it (a buffer overflow makes any
process a puppet), so leakage-resistance is a matter of degree, not
kind. The first wall is trusted processes not leaking tokens. The second
wall — the failsafe — is the **trust domain** (§4): a leaked kernel
token is unexercisable across a jail boundary.

## 0. Decisions log

- **No per-process cap tables, no handles, no untyped/retype.** The only
  kernel capability state is the global grant tree (§1). This buys
  authority hygiene and structural containment, not seL4-class
  verification, and the document says so plainly.
- **Tokens are bearer within a trust domain.** Accepted per the
  degree-not-kind argument; the domain wall is anchor-side and
  unconditional (§4), never a caveat an issuer might forget.
- **Attenuation is conjunctive.** A token's effective authority is the
  anchor grant's params ∩ every caveat. Chaining MACs (parent MAC keys
  the child link) makes narrowing one-way; conjunction makes userspace
  minting safe. Unknown caveat type → reject. Fail closed.
- **Caveat vocabulary is closed and tiny** (§2.2): per-type parameter
  narrowing plus PID binding. No expiry (no wall clock contract), no
  third-party caveats, no predicates. Extensions get new type codes.
- **Revocation authority = grant ancestry.** A grant may be revoked or
  dropped by its creator or by the creator of any ancestor grant. Reap
  revokes a zombie's created grants (§7). No separate revoke-tokens.
- **Sub-grants are where you buy revocation granularity.** A leaked
  attenuated token cannot be individually revoked — you revoke its
  anchor. Convention: sub-grant at every trust boundary, attenuate
  freely within one. The domain rule forces a sub-grant at exactly
  those boundaries anyway, so mechanics and policy align.
- **Middleman death does not sever delegation.** Tokens reference
  anchors, not holders; an intermediary dying leaves downstream tokens
  valid unless it created a sub-grant (then reap kills that subtree).
  Death-coupling is chosen per delegation by sub-grant placement.
- **Grant management is a scheme, not syscalls.** The capability system
  adds **zero syscalls** (the table stays at `SYS_VM_SIZE 18` /
  `SYS_MAX 19`); it changes two signatures (§5.1) and adds scheme -2.
  The scheme earns its ring under the "kernel delivers events back"
  rule via `KEV_CAP_GRANT_DEAD`, and multi-step revocation rides the
  existing bounded doorbell-drain discipline instead of a caller retry
  loop.
- **The authority ring is inherited, like stdin** (§6). The client
  library uses the cap ring it was given and only creates a kernel one
  (`VM_SHARE(base, KSCHEME_CAP)`) as the default. This one convention
  makes the entire authority interface interposable.
- **Per-boot key; nothing persists.** Tokens and grants are transport
  and runtime state; durable authority is a userspace concern (§8).
  MAC compares are constant-time; tags are 16 bytes.
- **Cap checks add to, never replace, kernel safety checks.** A devmem
  token does not exempt `umem_map_device` from validating that the
  range is genuinely non-RAM device memory.
- **Enumeration is deliberately partial** (§9): "what could this
  process exercise" is unanswerable in general (authority is bytes),
  but fully answerable per *domain* — which is where the question
  matters.

## 1. Grants

```c
// A revocation anchor. The global grant tree is the kernel's entire
// capability state — the boot roots plus one node per deliberate
// delegation/containment boundary. Guarded by g_umem (the control
// plane), so grant liveness can never disagree with process liveness
// or revocation in flight.
struct grant {
  uint64_t id;             // monotonic, never reused
  uint8_t  type;           // GRANT_DEVMEM / GRANT_IRQ_ROUTE / GRANT_IOMMU_DEV
  union {                  // effective params: parent's ∩ the creating
    struct { uint64_t base, len; uint32_t flags; } devmem;   // chain's caveats,
    struct { struct irq_route *route; } irq;                 // flattened at
    struct { uint16_t rid; } iommu_dev;                      // creation
  };
  uint64_t domain;         // exercise gate (§4)
  struct process *creator; // revocation authority + reap tie + KEV target
  struct grant *parent, *children, *sibling; // bounded-revoke tree
  bool dead;               // set under g_umem; verification checks the
                           // whole ancestor chain (depth is small)
};
```

Grant types v1 mirror the hardware authority gaps:

- **GRANT_DEVMEM {base, len, flags}** — authority to map any page-aligned
  subrange with any subset of `flags` (`VM_DEVICE_*`). Root covers all
  non-RAM platform space. `pcid` sub-grants or attenuates BAR ranges for
  drivers (pci-design's "future BAR-range capability"; ECAM authority is
  a devmem token over the ECAM window).
- **GRANT_IRQ_ROUTE {route}** — claim/bind/release one `irq_route`. Root
  is route-wildcard. MSI routes allocate hardware state, so they
  materialize via `KIRQ_MSI` (§5.2), which creates a child grant.
- **GRANT_IOMMU_DEV {rid}** — attach/detach that requester id to a
  caller-owned domain. Root is rid-wildcard, delegated init → `pcid`,
  which owns topology discovery. Replaces first-claim-wins; realizes
  iommu-design §5's capability placeholder (per-rid; groups after ACS).

## 2. Tokens

### 2.1 Wire format

```
token  = hdr { version u8, ncaveats u8, grant_id u64 }
         ‖ caveat[0..ncaveats)          // ncaveats ≤ 8
         ‖ mac[16]
caveat = { type u8, pad[7], p0 u64, p1 u64 }   // 24 bytes, fixed

mac_0 = MAC(k_boot, hdr)
mac_i = MAC(mac_{i-1}, caveat_i)        // parent MAC keys the child link
```

Total ≤ 218 bytes; parsing is fixed-offset arithmetic on bounded input —
this is the one new attacker-facing parser in the kernel and it is kept
trivial on purpose. A root token is a bare header + MAC, written by the
kernel (bootinfo, `KCAP_SUBGRANT`, `KIRQ_MSI`). Everything longer was
made in userspace by `gdoslib` `token.c`: appending a caveat needs only
the token itself (the parent MAC is the key), so attenuation is offline,
zero-syscall, and works across arbitrary delegation hops. Verification
recomputes the chain under `k_boot`, checks the anchor grant and all its
ancestors live, then intersects params.

### 2.2 Caveats (closed vocabulary)

| type | p0, p1 | meaning (conjunctive) |
|---|---|---|
| CAV_DEVMEM_RANGE | base, len | effective range ∩= [base, base+len) |
| CAV_DEVMEM_FLAGS | mask, — | effective flags &= mask |
| CAV_IRQ_GSI | gsi, — | route must be this GSI |
| CAV_IOMMU_RID | rid, — | requester must be this rid |
| CAV_PID | pid, — | exerciser/presenter must be this process |

`CAV_PID` is opt-in possession binding (pids are never reused, §7): it
turns a bearer token into one useless anywhere but the named process —
for grants where even same-domain leakage is unacceptable. A caveat that
empties the intersection makes the token valid-but-useless, not invalid.

## 3. Exercise

Exercise sites verify tokens where the objects live; nothing about
enforcement moves to the channel (interposing *enforcement* would be
forgery, §6):

- **SYS_VM_MAP_DEVICE (token_ptr, token_len, flags)** — signature
  change: the token's effective devmem range *is* the mapping (narrow in
  userspace instead of passing base/len), `flags` ⊆ effective flags.
  Existing kernel range validation (non-RAM, alignment, FIRMWARE only on
  ACPI backing) unchanged on top.
- **IRQ ring**: `KIRQ_CLAIM` / `KIRQ_BIND` / `KIRQ_RELEASE` present a
  route token by offset+length *within the ring's block* (`sqe->a` =
  offset, `sqe->b` = length, `sqe->c` = cookie where one applies),
  replacing bare GSIs / granted route ids. `KIRQ_ACK` stays token-free:
  acking is scoped to a route already bound to this ring — the ring is
  the authority context and the hot path stays a raw indexed op.
  Authority is spent at claim/bind, not per interrupt.
- **IOMMU ring**: `KIOMMU_DEVICE_ATTACH/DETACH` present a device token
  the same way (the "requester field becomes a capability" swap
  iommu-design §5 reserved). `DOMAIN_*` and `*_MAP_BLOCK` are unchanged:
  domains are self-owned and mapping is already restricted to the
  caller's own ublocks — possession-based by construction.

Exercise additionally requires `caller.domain == anchor.domain` (§4).
V1 has no hot path carrying tokens — every token-checked op is
setup-time — so per-exercise HMAC cost is irrelevant.

## 4. Trust domains

A flat `uint64_t` label on every process: inherited at `PROC_CREATE`,
immutable for life, never reused. Init is domain 0.
`SYS_PROC_CREATE(PROC_NEW_DOMAIN)` starts the embryo in a fresh domain —
that is the entire lifecycle: no kernel object, nothing to reap, jails
nest for free, and **any unmodified program can run jailed**.

Two anchor-side rules, no token cooperation required:

1. **Exercise**: the caller's domain must equal the anchor grant's.
2. **Crossing**: `KCAP_SUBGRANT` may target another domain — creating a
   grant is precisely the deliberate act that arms a jail.

A leaked token is inert outside its anchor's domain: the second wall
holds even when the first (nobody leaks) fails. Because crossings work
on *running* processes, a jail can be granted new authority after birth
— by its supervisor's explicit sub-grant, never by drift. Within a
domain, tokens flow with zero ceremony, which is the common case: the
base system is one domain. (Drivers jailed from *each other* is
supported — spawn each with `PROC_NEW_DOMAIN` and sub-grant its devmem/
route/rid tokens into its domain — at the price of a grant per driver.)

## 5. The cap channel (`KSCHEME_CAP = -2`)

Grant management is a kernel scheme, taking the -2 slot (current
assignments: -1 shares, -3 tree, -4 IRQ, -6 IOMMU). Creation policy is
open, like every scheme: a cap ring conveys nothing by itself.

```c
#define KCAP_SUBGRANT 1 // a = token off/len (32/32), b = target domain,
                        //   c = output offset for the new root token
                        // completion: a = grant id
#define KCAP_REVOKE   2 // a = token off/len — kill the anchor's DESCENDANT
                        //   grants; CQE posts when the subtree is gone
#define KCAP_DROP     3 // a = token off/len — kill the anchor itself;
                        //   SYSERR_EXIST while it has children
#define KCAP_QUERY    4 // a = token off/len — completion: effective params
                        //   + liveness (debugging, pre-flight validation)

#define KEV_CAP_GRANT_DEAD KEV(7) // to the creator's cap ring when an
                        // ancestor revocation or creator reap kills a
                        // grant it created; a = grant id. Posted with
                        // the KEV_SHARE post-or-replay-bit discipline.
```

Tokens ride inside the ring's block by offset+length; `SUBGRANT` writes
the new root token back into the block at the caller-chosen offset.
Authority checks: `SUBGRANT` needs only a valid chain (the new grant's
params are the chain's flattened effective params — sub-granting also
compresses long chains); `REVOKE`/`DROP` need the caller to be the
anchor's creator or an ancestor grant's creator.

`KCAP_REVOKE` improves on a syscall retry loop: the SQE simply stays in
flight across doorbell drains — one bounded chunk of subtree teardown
per drain within the existing `RING_SQ_BATCH` discipline — and the CQE
posts on completion. Same bounded-work invariant, no caller-driven
`REAP_MORE` dance. Grants whose objects hold live claims (a revoked
route grant whose route is claimed to a ring) tear the claim down in the
same step via the object's existing bounded teardown hook.

### 5.1 ABI delta, complete

- New scheme -2 with the ops/event above.
- `SYS_VM_MAP_DEVICE` signature: `(token_ptr, token_len, flags)`.
- `SYS_PROC_CREATE` gains `flags` (`PROC_NEW_DOMAIN 1u`).
- Bootinfo gains a section of literal root token bytes (§7).
- **No new syscalls.** The table stays `SYS_VM_SIZE 18`, `SYS_MAX 19`.

### 5.2 MSI

`KIRQ_MSI` presents a parent route token (offset/len packed in `a`,
output offset in `b`): the kernel allocates a free route, creates a
child grant under the presented anchor (creator = caller, domain =
caller's), and writes a root token for it into the ring block; the
completion carries the MSI address/data pack. pcid programs the device
(its job per irq-design) and hands the token bytes to the driver over
their ring — or sub-grants it into the driver's domain if the driver is
jailed. The old "target must be a direct child" rule is deleted; pcid
and drivers can be siblings.

## 6. Interposition

Convention: **a process receives its authority ring like it receives
stdin.** The gdoslib client uses the cap ring it was handed, creating a
kernel one only as the default when none was given. Everything the ring
carries is SQEs and token bytes, so a jail supervisor can service it
instead of the kernel: filtering, logging, rewriting, or satisfying
requests by making narrowed `KCAP_SUBGRANT`s against its own broader
tokens and forwarding the results. The jail cannot tell, and the
supervisor gains no forgery power — it can only re-delegate what it
already holds. This is Genode-style parent interposition falling out of
the IPC design, and it composes with domains: the domain wall stops
leaked-token exercise; the interposed ring shapes what a jail can even
request.

Enforcement does not interpose: exercise sites (§3) verify against
`k_boot` at the object, always. Full kernel-surface virtualization
(govindos-in-govindos) would need the receive-don't-create convention
extended to IRQ/IOMMU rings too; the cap ring deliberately sets that
precedent, but this is an observation, not a v1 goal.

## 7. Death, reap, bootstrap

- **Reap** gains one step type: revoke one grant created by the zombie,
  deepest-first, before owned blocks are freed (a route grant's teardown
  may release a claim on a ring living in an owned block — verify this
  ordering against `channel_block_destroyable` sequencing at
  implementation time). Grants die with their creators; tokens die with
  their grants; no other cleanup exists because no other state exists.
- **Pids are never reused** (monotonic 64-bit): `CAV_PID` and `creator`
  ties require it; the phase-2 identity split (§10) wants it anyway.
- **Bootstrap**: the kernel creates the three root grants (creator =
  init, domain 0) and writes their root tokens as bytes into a new
  bootinfo section. No pre-seal table population, no minting hook —
  init reads bytes and starts delegating over rings. Boot selftests
  drive the grant tree through kernel-side calls as usual.

## 8. Userspace tokens

The larger half of the design is deliberately not kernel code, and now
differs from the kernel half only in *who holds the key*. Servers (a
future bootfs/fsd, netd, pcid's topology queries) issue tokens for their
own objects under their own secrets using the same `gdoslib token.c`
(issue / attenuate / verify), the same wire format, the same conjunctive
caveat discipline — with server-defined caveat vocabularies (path
prefix, read-only, byte quota, expiry-by-epoch) the kernel never sees.
Server-side revocation is the server's state: per-object epochs
(coarse, stateless) or issuance denylists (fine). A `CAV_PID`-style
peer-binding caveat is available where bearer-ness is unacceptable —
the server knows its peer from the share edge.

Stated plainly: userspace authority is bearer authority by design,
bounded by domain walls only if servers choose to check domains (the
kernel does not check for them). That is the accepted trade for zero
kernel involvement, offline attenuation, and portable server semantics.

## 9. Invariants

- Kernel capability state is exactly the grant tree: small, enumerable
  under `g_umem`, every node deliberately created, every node revocable
  in bounded steps.
- Effective authority only ever shrinks: along caveat chains
  (conjunction), along sub-grants (flattened intersection), never
  otherwise.
- A domain's exercisable kernel authority is enumerable: the grants
  labeled with it. "What could process P do" is deliberately not
  answerable below domain granularity — authority between anchors is
  bytes.
- A leaked token is: dead if its anchor chain is dead, inert outside
  its anchor's domain, useless outside `CAV_PID` if bound, and
  otherwise usable only by same-domain processes — which could have
  proxied through the leaker anyway (degree, not kind).
- Every cap operation is bounded; unbounded teardown is chunked across
  doorbell drains with a CQE at the end.
- No token check replaces a safety check.

## 10. Deferred

- **Process authority (phase 2).** KILL/REAP/SPAWN/MOVE_IN/PROTECT are
  still pid+tree-checked. The token translation: process-object grants
  with rights caveats, `GETPID`-style ids staying pure identity. Costs
  parked: every proc syscall changes shape; kernel events need
  grant-cookie plumbing; processes would be the first grant *params*
  that die, needing object→grant back-references. Reap authority stays
  parent-only regardless — reap decides where salvaged blocks go (tree
  edges), not just who may trigger it.
- **Group grants / ACS.** GRANT_IOMMU_DEV is per-rid until pci-design's
  ACS work defines isolation groups.
- **Persistence.** `k_boot` is per-boot by design; durable authority is
  re-issued by servers / re-delegated by init from the root grants.
- **Full receive-don't-create virtualization** of scheme rings (§6).

## 11. Implementation order

1. Grant tree under `g_umem` (create/link/dead-mark/ancestor-liveness);
   token verifier (parse, chain MAC, conjunctive intersection) +
   `k_boot`; selftests including malformed-token fuzzing.
2. `gdoslib token.c` (issue/attenuate/verify — shared by kernel
   selftests and future servers).
3. Root grants + bootinfo token section; domain label on
   `struct process` + `PROC_CREATE(flags)` / `PROC_NEW_DOMAIN`.
4. Scheme -2: SUBGRANT/DROP/QUERY, then multi-drain REVOKE +
   `KEV_CAP_GRANT_DEAD`; reap step for creator death. Selftests: stale
   anchor, dead ancestor, cross-domain exercise/subgrant, revoke racing
   exercise.
5. Convert `SYS_VM_MAP_DEVICE` to token signature; delete the umem.c
   TODO; pcid attenuates driver sub-ranges.
6. Convert IRQ scheme ops (CLAIM/BIND/RELEASE by token; MSI per §5.2);
   delete both irq.c TODOs and `route->target`.
7. Convert `KIOMMU_DEVICE_ATTACH/DETACH`; delete the iommu.c
   first-claim hook; update nvmed + QEMU harness.
8. Authority-ring inheritance convention in gdoslib; jail-supervisor
   interposition selftest.
9. Docs pass: retire the "trusted userspace v1" caveats in
   pci-design/iommu-design; note divergences here.
