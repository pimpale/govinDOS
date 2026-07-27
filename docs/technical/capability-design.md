# Capability design: grants, tokens, and the cap channel

Status: **implemented 2026-07-16 from the design planned 2026-07-14;
trust domains and offline caveat chains remain deferred (§9). Final v1 shape: kernel
state is a tree of revocation anchors ("grants"); authority is
fixed-size MAC'd tokens transferred as plain bytes; all attenuation is
re-issuance over the cap scheme (`KSCHEME_CAP = -2`); zero new
syscalls. Revocation is prospective, grant depth is capped at 64, and
pid binding, grant-death events, and resource quotas are deferred.**
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

So there is **one idiom**: authority is token bytes, copied over rings
with no kernel involvement. Userspace servers verify tokens for their
own objects under their own secrets (§7). The kernel verifies tokens for
kernel objects under the boot key — it is just another server that
happens to enforce at the hardware. Because grant *management* also
happens over a channel (§4), the kernel's authority service is
**proxyable**: an interposing supervisor can filter and re-delegate
requests, with the ownership differences stated in §5.

Delegation has exactly two verbs, with a clean cost split:

- **Copy the bytes**: shares *the same* authority. Free, stateless,
  offline; both holders stand behind one anchor and are revoked
  together.
- **Re-issue** (`KCAP_SUBGRANT`): mints *distinct, narrowed* authority —
  a new grant node. Costs kernel state and a ring round-trip; buys
  individual revocability, enumerability, and death-coupling to the
  issuer.

That is the deliberate cost model: *revocability is what kernel state is
for* — and in v1, attenuation and revocability are the same purchase.
(Macaroon-style offline attenuation, which decouples them, is designed
and deferred — §9.)

Containment is honest about its limits: no design stops a token holder
from proxying for a peer that owns it (a buffer overflow makes any
process a puppet), so leakage-resistance is a matter of degree, not
kind. The first wall is trusted processes not leaking tokens. V1's
backstop when that fails is anchor revocation. A second, structural wall
— **trust domains**, which make a
leaked kernel token unexercisable across a jail boundary — is fully
designed but deferred (§9).

## 0. Decisions log

- **No per-process cap tables, no handles, no untyped/retype.** The only
  kernel capability state is the global grant tree (§1). This buys
  authority hygiene and structural containment, not seL4-class
  verification, and the document says so plainly.
- **Tokens are bearer in v1.** Accepted per the degree-not-kind argument:
  any process a holder can talk to could proxy through it anyway.
  Containment is prospective revocation until trust domains land (§9).
- **All attenuation is re-issuance.** Narrowing happens only at
  `KCAP_SUBGRANT`, where the kernel enforces new params ⊆ parent params
  at one choke point. No caveat chains, no in-token predicates, no
  chain-walk verification: a token is a fixed 32-byte record and the
  kernel's attacker-facing parser is a bounds check plus one
  constant-time HMAC. Chosen for v1 simplicity; offline attenuation is
  deferred, not rejected (§9), and the wire format reserves room for it.
- **Every narrowing is its own anchor.** Re-issuance makes revocation
  granularity perfect — each delegated narrowing is individually
  revocable — at the cost of a grant node per attenuation. V1 caps grant
  depth at 64. A breadth quota is deferred; untrusted token holders can
  otherwise exhaust kernel memory by creating sibling grants, an accepted
  trusted-v1 limitation.
- **Narrowed delegation death-couples; copied tokens don't.** A
  sub-grant dies with its creator (reap revokes it), severing that
  delegation. Copying bytes shares the original anchor and outlives the
  middleman. The two verbs make death-coupling explicit rather than a
  convention.
- **Revocation authority = grant ancestry.** A grant may be revoked by
  its creator or by the creator of any ancestor grant. Revocation marks
  the anchor dead immediately, then reclaims its subtree in bounded retry
  steps. Reap
  revokes a zombie's created grants (§6). No separate revoke-tokens.
- **Grant management is a scheme, not syscalls.** The capability system
  adds **zero syscalls** (the table stays at `SYS_VM_SIZE 18` /
  `SYS_MAX 19`); it changes one signature (§4.1) and adds scheme -2.
  The scheme earns its ring under the "kernel delivers events back"
  rule as a command-oriented exception: grant-death events are deferred.
  Multi-step revocation returns `SYSERR_AGAIN`; the client resubmits until
  cleanup completes, matching the existing bounded-retry idiom.
- **The authority ring is inherited, like stdin** (§5). The client
  library uses the cap ring it was given and only creates a kernel one
  (`VM_SHARE(base, KSCHEME_CAP)`) as the default. This one convention
  makes the authority request interface interposable (§5).
- **Per-boot key; nothing persists.** Tokens and grants are transport
  and runtime state; durable authority is a userspace concern (§7).
  MAC compares are constant-time; tags are 16 bytes.
- **Cap checks add to, never replace, kernel safety checks.** A devmem
  token does not exempt `umem_map_device` from validating that the
  range is genuinely non-RAM device memory.
- **Revocation is prospective.** Once an exercise succeeds, revoking its
  grant does not undo the resulting devmem mapping, IOMMU attachment, or
  IRQ binding. Authority-reducing cleanup operations are authorized by
  the already-established ring/object association, not by a live token.
- **Enumeration is deliberately partial** (§8): "what could this
  process exercise" is unanswerable — tokens are bytes. What stays
  enumerable is the grant tree — which, with re-issuance, records
  *every* narrowing, not just deliberate boundaries — plus all
  exercised object state. Trust domains will restore
  potential-authority enumeration at jail granularity (§9).

## 1. Grants

```c
// A revocation anchor. The global grant tree is the kernel's entire
// capability state — the boot roots plus one node per re-issued
// (narrowed) delegation. Guarded by g_umem (the control plane), so
// grant liveness can never disagree with process liveness or
// revocation in flight.
//
// Ownership: grants are individually allocated and linked into intrusive
// parent/child and per-creator lists. Nodes never move. The global
// id→grant llrb resolves token grant_id values (pid-registry style).
// Removal order, under g_umem: unlink the non-owning references,
// then destroy the owning node. A per-creator breadth quota is deferred.
struct grant {
  uint64_t id;             // monotonic, never reused
  uint8_t  type;           // GRANT_DEVMEM / GRANT_IRQ_ROUTE / GRANT_IOMMU_DEV
  uint8_t  depth;          // root 0; SUBGRANT rejects parent depth == 64
  bool wildcard;           // only boot roots are wildcard grants
  bool dead;               // invalid immediately; retained during cleanup
  union {                  // params, ⊆ parent's (enforced at SUBGRANT)
    struct { uint64_t base, len; uint32_t flags; } devmem;
    struct { struct irq_route *route; uint32_t id; } irq; // generation-bound
    struct { uint32_t requester; } iommu_dev; // segment:bus:devfn
  };
  struct process *creator; // revocation authority + reap tie
  struct grant *parent;    // nullptr for the boot roots
  struct grant *first_child, *next_sibling, *prev_sibling;
  struct grant *creator_next, *creator_prev;
};
```

Bounded revoke follows `first_child` to one deepest leaf (at most 64
links) and removes it. Intrusive sibling and creator links make removal
O(1), with no container growth or copying under `g_umem`.

(Trust domains add a `domain` field here when they land — §9.)

Grant types v1 mirror the hardware authority gaps:

- **GRANT_DEVMEM {base, len, flags}** — authority to map the range with
  any subset of `flags` (`VM_DEVICE_*`). Root covers all non-RAM
  platform space. `pcid` sub-grants BAR ranges for drivers (pci-design's
  "future BAR-range capability"; ECAM authority is a devmem grant over
  the ECAM window).
- **GRANT_IRQ_ROUTE {route}** — claim/bind/release one `irq_route`. Root
  is route-wildcard; sub-granting selects a GSI. MSI routes allocate
  hardware state, so they materialize via `KIRQ_MSI` (§4.2), which
  creates a child grant.
- **GRANT_IOMMU_DEV {requester}** — attach/detach that full PCI
  segment:bus:devfn requester id to a caller-owned domain. Root is a
  tagged requester-wildcard, delegated init → `pcid`,
  which owns topology discovery. Replaces first-claim-wins; realizes
  iommu-design §5's capability placeholder (per-rid; groups after ACS).

## 2. Tokens

```
token = { version u8, nres u8 (must be 0; reserved for future caveat
          count, §9), reserved[6] (must be zero), grant_id_le u64 }
        ‖ mac[16]                                  // 32 bytes, fixed
mac   = first_16_bytes(HMAC-SHA-256(k_boot,
                                    "govindos-cap-v1" ‖ hdr))
```

A token is a bearer reference to one grant — nothing more. The kernel
writes them (bootinfo, `KCAP_SUBGRANT`, `KIRQ_MSI`); userspace only
copies them. Verification: fixed-size bounds check, recompute the MAC
under the per-boot key `k_boot`, constant-time compare, check the grant
and all its ancestors live. The ancestor walk is bounded by the depth-64
limit. One HMAC, no
parsing loop — this is the entire attacker-facing surface.

Effective authority is the grant's params, full stop. To hand someone
less than you hold, `KCAP_SUBGRANT` a narrowed child and give them that
child's token (§4). To hand someone exactly what you hold, copy the
bytes — knowing you share one anchor and one revocation fate.

## 3. Exercise

Exercise sites verify tokens where the objects live; nothing about
enforcement moves to the channel (interposing *enforcement* would be
forgery, §5):

- **SYS_VM_MAP_DEVICE (token_ptr, token_len, flags)** — signature
  change: the grant's devmem range *is* the mapping (narrow via
  SUBGRANT instead of passing base/len), `flags` ⊆ grant flags.
  Existing kernel range validation (non-RAM, alignment, FIRMWARE only
  on ACPI backing) unchanged on top.
- **IRQ ring**: `KIRQ_CLAIM` / `KIRQ_BIND` present a
  route token by offset+length *within the ring's block* (`sqe->a` =
  offset, `sqe->b` = length, `sqe->c` = cookie where one applies),
  replacing bare GSIs / granted route ids. `KIRQ_ACK` and `KIRQ_RELEASE`
  stay token-free: both are scoped to a route already bound to this ring.
  The ring is the authority context and the hot/cleanup paths stay raw
  indexed ops.
  Authority is spent at claim/bind, not per interrupt.
- **IOMMU ring**: `KIOMMU_DEVICE_ATTACH` presents a device token the same
  way (the "requester field becomes a capability" swap
  iommu-design §5 reserved). `DOMAIN_*` and `*_MAP_BLOCK` are unchanged:
  domains are self-owned and mapping is already restricted to the
  caller's own ublocks — possession-based by construction.
  `KIOMMU_DEVICE_DETACH` stays requester-id based and succeeds only for an
  attachment already owned by a domain on this ring, so revocation cannot
  prevent authority-reducing cleanup.

V1 has no hot path carrying tokens — every token-checked op is
setup-time — so per-exercise MAC cost is irrelevant.

## 4. The cap channel (`KSCHEME_CAP = -2`)

Grant management is a kernel scheme, taking the -2 slot (current
assignments: -1 shares, -3 tree, -4 IRQ, -6 IOMMU). Creation policy is
open, like every scheme: a cap ring conveys nothing by itself.

```c
// SUBGRANT request, at sqe->a's offset in the ring block. Params are
// type-interpreted and must be ⊆ the parent grant's. `present` avoids
// overloading zero, which is a valid base, GSI, and requester id.
struct kcap_subgrant_req {
  uint64_t token_off, token_len; // parent token, within this block
  uint64_t p0, p1, p2;           // devmem: base, len, flags-mask
                                 // irq: gsi; iommu: requester id
  uint32_t present;              // KCAP_PARAM_P0/P1/P2
  uint32_t reserved;             // must be zero
  uint64_t reserved2;            // must be zero
};

#define KCAP_PARAM_P0 1u
#define KCAP_PARAM_P1 2u
#define KCAP_PARAM_P2 4u

#define KCAP_SUBGRANT 1 // a = request offset, b = reserved (0; becomes
                        //   the target trust domain, §9), c = output
                        //   offset for the child's token
                        // completion: a = grant id
#define KCAP_REVOKE   2 // a = token offset, b = token length. First call
                        //   marks the anchor dead immediately; each call
                        //   reclaims bounded work. SYSERR_AGAIN means
                        //   resubmit; success means the subtree is gone.
#define KCAP_QUERY    3 // a = request offset, c = result offset
```

Tokens and requests ride inside the ring's block; `SUBGRANT` writes the
child's token back at the caller-chosen offset. Authority checks:
`SUBGRANT` needs a live parent token (and creates the child with creator =
caller); `REVOKE` accepts an authenticated live or dead-but-retained token
and needs the caller to be the anchor's creator or an ancestor grant's
creator. Dead grants remain in the id index until their final cleanup step
so retries can resolve them. `QUERY` writes a fixed result structure into
the ring block rather than trying to squeeze type-specific params into a
CQE.

On its first call, `KCAP_REVOKE` sets `dead` before returning: verification
of the anchor or any descendant fails from that instant. It then frees at
most one deepest leaf per call. A completion with `SYSERR_AGAIN` means one
bounded cleanup step completed and the caller resubmits the same request;
success means the anchor itself was removed. Revocation does not tear down
authority already exercised through a live token (§3).

### 4.1 ABI delta, complete

- New command-only scheme -2 with the ops above.
- `SYS_VM_MAP_DEVICE` signature: `(token_ptr, token_len, flags)`.
- Bootinfo v2 gains three literal root token fields (§6).
- **No new syscalls.** The table stays `SYS_VM_SIZE 18`, `SYS_MAX 19`.

(Trust domains later add a `SYS_PROC_CREATE` flags argument and give
`KCAP_SUBGRANT`'s reserved `b` its meaning; caveat chains later give
the token's reserved `nres` byte its meaning — all additive.)

### 4.2 MSI

`KIRQ_MSI` presents a parent route token (`a` = offset, `b` = length,
`c` = output offset): the kernel allocates a free route, creates a
child grant under the presented anchor (creator = caller), and writes
its token into the ring block; the completion carries the MSI
address/data pack. The concrete grant records the route generation as
well as its slot, so releasing and reusing a slot cannot revive an old
token. pcid programs the device (its job per irq-design)
and hands the token bytes to the driver over their ring — or, for a
driver it wants revocable independently, sub-grants first and hands
over the child's token. The old "target must be a direct child" rule
is deleted; pcid and drivers can be siblings.

## 5. Interposition

Convention: **a process receives its authority ring like it receives
stdin.** The gdoslib client uses the cap ring it was handed, creating a
kernel one only as the default when none was given. Everything the ring
carries is SQEs and token bytes, so a jail supervisor can service it
instead of the kernel: filtering, logging, rewriting, or satisfying
requests by making narrowed `KCAP_SUBGRANT`s against its own broader
tokens and forwarding the results. The supervisor gains no forgery power
— it can only re-delegate what it already holds. This proxy is
intentionally not ownership-transparent: kernel grants it creates are
death-coupled to the supervisor, and the supervisor owns their revocation
authority. Until trust domains land this is an authority-filtering
convention, not a containment wall.

Enforcement does not interpose: exercise sites (§3) verify against
`k_boot` at the object, always. Full kernel-surface virtualization
(govindos-in-govindos) would need the receive-don't-create convention
extended to IRQ/IOMMU rings too; the cap ring deliberately sets that
precedent, but this is an observation, not a v1 goal.

## 6. Death, reap, bootstrap

- **Reap** gains one step type: advance revocation of one grant created by
  the zombie,
  deepest-first, before the parent reclaims owned blocks (a route grant's
  teardown may release a claim on a ring living in an owned block — verify
  this ordering against `channel_block_destroyable` sequencing at
  implementation time). Grants die with their creators; tokens die with
  their grants; no other capability cleanup exists because no other
  capability state exists. Established mappings, claims, and attachments
  retain their ordinary object lifetimes.
  - Reap no longer frees the zombie's memory
    ([ipc-process-design.md](ipc-process-design.md) §4) — the parent claims
    each block with `VM_MOVE` and frees it itself — so grant revocation must
    complete before the parent's reclaim, not before a kernel-side free.
- **TODO — grant enumeration.** `SYS_VM_FREE` refuses a block that still
  anchors grants and does not revoke them itself
  ([memory-design.md](memory-design.md) §5). Userspace therefore needs to
  ask which grants a block anchors — a resumable read over the creator's
  grant list, plausibly a `KCAP_LIST` op on the `-2` ring. Not yet
  designed; until it exists, a caller learns only that the free failed.
- **Pids are never reused** (monotonic 64-bit): creator ties require it;
  the phase-2 identity split (§9) wants it
  anyway.
- **Bootstrap**: the kernel creates the three root grants (creator =
  init) and writes their root tokens as bytes into a new bootinfo
  section. No pre-seal table population, no minting hook — init reads
  bytes and starts delegating over rings. Boot selftests drive the
  grant tree through kernel-side calls as usual.

## 7. Userspace tokens

The larger half of the design is deliberately not kernel code, and
differs from the kernel half only in *who holds the key and the state*.
Servers (a future bootfs/fsd, netd, pcid's topology queries) can issue
tokens for their own objects under their own secrets using a future
`gdoslib token.c` issue/verify helper and the same re-issuance discipline:
a client wanting narrower authority for a peer asks the server to
re-issue, and the server records its own grant-equivalent (an entry in
its own table — servers have hash maps; this is not a hardship).
Server-side revocation is that state's deletion, or per-object epochs
where bulk invalidation is enough. Peer-binding is available where
bearer-ness is unacceptable — the server knows its peer from the share
edge. Servers that later want offline attenuation can adopt caveat
chains (§9) unilaterally, without kernel involvement — the formats are
theirs.

Stated plainly: userspace authority is bearer authority by design. That
is the accepted trade for zero kernel involvement and portable server
semantics; servers that need more bind to peers or expire aggressively.

## 8. Invariants

- Kernel capability state is exactly the grant tree: enumerable under
  `g_umem`, one node per deliberate re-issuance, depth at most 64, and
  revocable in bounded retry steps. Breadth is not quota-limited in v1.
- Params only ever shrink along the grant tree (⊆ enforced at
  `SUBGRANT`), and a token confers exactly its grant's params — never
  more, never other.
- "What could process P do" is deliberately not answerable — tokens
  are bytes. What is answerable: every narrowing ever issued (the
  grant tree) and everything exercised (object state).
- A leaked token is dead for future exercise if its anchor chain is dead,
  and otherwise bearer. Revocation does not undo authority exercised before
  the anchor died. (Trust domains, §9, add: inert outside its anchor's
  domain.)
- Every cap operation is bounded; subtree teardown advances one bounded
  step per `KCAP_REVOKE`, returning `SYSERR_AGAIN` until complete.
- No token check replaces a safety check.

## 9. Deferred

- **Trust domains — the containment failsafe.** Fully designed, deferred
  as a unit; nothing in v1 blocks it (the `grant.domain` field and
  `KCAP_SUBGRANT.b` are reserved). The design, for the record: a flat
  `uint64_t` label on every process — inherited at `PROC_CREATE`,
  immutable for life, never reused; init is domain 0;
  `SYS_PROC_CREATE(PROC_NEW_DOMAIN)` starts a fresh one (no kernel
  object, nothing to reap, jails nest, any unmodified program can run
  jailed — the reason domains beat a per-process no-adopt bit). Two
  anchor-side rules, deliberately not token content so no issuer can
  forget them: exercise requires `caller.domain == anchor.domain`, and
  `KCAP_SUBGRANT` targeting another domain is the one crossing — an
  explicit act that works on *running* jails. Restores
  potential-authority enumeration at jail granularity ("what could this
  jail ever exercise" = the grants labeled with its domain) and makes
  leaked tokens inert across jail walls. Until then, authority filtering =
  interposed authority rings (§5) + prospective revocation.
- **PID binding.** A future `bound_pid` grant field can restrict exercise
  to one never-reused pid. Binding must be monotonic: a child of a bound
  grant remains bound to the same pid. V1 has no binding field or
  binding behavior.
- **Grant quotas.** Depth is bounded at 64 in v1; per-process or
  parent-donated breadth budgets remain future work.
- **Grant-death events.** A future cap-ring event may notify a creator when
  an ancestor kills its grant. It requires persistent tombstone/pending
  event state and is deliberately absent from v1.
- **Offline attenuation (macaroon caveat chains).** Designed during this
  document's second revision, deferred in favor of re-issuance-only:
  tokens grow HMAC-chained caveat records (`mac_i = MAC(mac_{i-1},
  caveat_i)` — the parent MAC keys the child link, so holders attenuate
  offline and can never widen), evaluated conjunctively against the
  anchor's params from a closed vocabulary (range/flags/gsi/rid/pid).
  The token's reserved `nres` byte becomes the caveat count; verifiers
  gain a bounded chain walk. Worth revisiting when delegation chains
  get long or high-frequency enough that a ring round-trip per
  narrowing hurts — the trade is a real in-kernel parser and the loss
  of one-anchor-per-narrowing enumerability, which is why v1 skips it.
- **Process authority (phase 2).** KILL/REAP/SPAWN/MOVE_IN/PROTECT are
  still pid+tree-checked. The token translation: process-object grants
  with rights params, ids staying pure identity. Costs parked: every
  proc syscall changes shape; kernel events need grant-cookie plumbing;
  processes would be the first grant *params* that die, needing
  object→grant back-references. Reap authority stays parent-only
  regardless — reap decides where salvaged blocks go (tree edges), not
  just who may trigger it.
- **Group grants / ACS.** GRANT_IOMMU_DEV is per-rid until pci-design's
  ACS work defines isolation groups.
- **Persistence.** `k_boot` is per-boot by design; durable authority is
  re-issued by servers / re-delegated by init from the root grants.
- **Full receive-don't-create virtualization** of scheme rings (§5).

## 10. Implementation order

1. Intrusive grant tree under `g_umem`
   (create/link/dead-mark/ancestor-liveness, id→grant llrb, creator
   bookkeeping); token write/verify (fixed record, one HMAC) + `k_boot`;
   selftests including the SHA-256 vector, forged tokens, zero-base
   narrowing, the depth boundary, and prospective retry revocation.
2. Root grants + bootinfo token section.
3. Scheme -2: SUBGRANT (⊆ enforcement and depth bound) / QUERY /
   retryable REVOKE; reap step for creator death. Selftests: stale anchor,
   dead ancestor, immediate invalidation before cleanup completes,
   depth-64 rejection, zero-valued selectors, and params-widening rejection.
4. Convert `SYS_VM_MAP_DEVICE` to token signature; delete the umem.c
   TODO; pcid sub-grants driver BAR ranges.
5. Convert IRQ scheme ops (CLAIM/BIND by token; ring-scoped RELEASE; MSI
   per §4.2);
   delete both irq.c authorization TODOs. `route->target` remains only as
   the unbound MSI allocation-lifetime owner; it is not bind authority.
6. Convert `KIOMMU_DEVICE_ATTACH` and retain ring-scoped DETACH; delete the iommu.c
   first-claim hook; update nvmed + QEMU harness.
7. Authority-ring inheritance/default convention in gdoslib.
8. Docs pass: retire the "trusted userspace v1" caveats in
   pci-design/iommu-design; note divergences here.

Trust domains and caveat chains (§9) slot in later without renumbering:
`domain` field + `PROC_CREATE` flags + `SUBGRANT.b`, and the token
`nres` byte, respectively — additive throughout.
