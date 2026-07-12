# IRQ design: userspace drivers, the irq scheme

Status: **pin IRQs implemented 2026-07-11; MSI remains phase 2.** Companion to
[ipc-process-design.md](ipc-process-design.md); this is the first of the
"device schemes, where the IRQ handler posts the completion CQE" its §2
promised, and it must obey that document's design law: bounded
non-blocking commands, registrations for interest, events for results,
no kernel threads, no deferred kernel work.

Planned implementation map: `abi/gdos/kring_irq.h` (scheme id, ops,
events), `kernel/src/schemes/irq.c` (claims, ack protocol, replay,
route table), `kernel/archsrc/x86_64/ioapic.c` (new: RTE
program/mask/unmask, MADT parse), `interrupts.c` (device-vector
dispatch branch), `lib/sys/kring.c` (driver-side wrappers).

## 0. Decisions log

- **IRQ delivery is a kernel scheme (`-4`), not a syscall family.** A
  driver claims interrupt lines by submitting SQEs on an irq ring and
  hears about interrupts as event CQEs on the same ring. No new
  syscalls: claim/release/ack are ring ops, waiting is
  `SYS_BLOCK_WAIT` or a wait-group registration like any other
  channel. Redox's `irq:` scheme is the precedent for the shape;
  govindos already has the transport.
- **The IRQ handler posts the CQE itself — borrowed context, stripe
  plane only.** There is no kernel thread and nothing to defer to, and
  none is needed: interrupts only ever arrive in IF=1 contexts (ring 3
  or the scheduler's idle window — `spinlock_lock` runs IRQs-off and
  IA32_FMASK masks IF for the whole syscall path), so the handler
  holds no locks on entry and may take stripe/scheduler spinlocks. It
  must **never take `g_umem`** (a shootdown-servicing lock; a handler
  spinning on it IRQs-off can be the very shootdown target its holder
  is waiting on). The post is architecturally a data-plane producer —
  the same locking shape as a peer's doorbell (§4).
- **Count/ack protocol, Redox semantics with Managarm's race analysis.**
  Per claim the kernel keeps `raised` (bumped by the handler) and
  `acked` (set by the driver's `KIRQ_ACK`). An interrupt is *pending*
  while `raised != acked` — level state exactly like un-notified share
  edges, replayed into the CQ when slots free. An ack that doesn't
  cover a raise that slipped in re-fires the event instead of being
  rejected: Managarm's stale-ack rejection made benign (§3).
- **Masking policy follows trigger mode, not driver choice.** Level
  lines are masked in the handler and unmasked by `KIRQ_ACK` (the
  MINIX/Managarm mask-in-service rule — a level line must stay masked
  until the device is quiesced or it screams). Edge/MSI never mask;
  the counter carries coalescing. Trigger mode comes from
  ACPI (MADT source overrides, `_PRT`), never from the claimant.
- **Exclusive claims only; sharing is deferred.** One ring per line
  (`SYSERR_EXIST` on a second claim). Shared legacy lines need
  per-sink ack bits, nack, and stall-backoff (MINIX `irq_actids`,
  Managarm ack-wins/all-nack-stalls) — real machinery that modern
  hardware makes optional, because everything that matters is MSI.
  If it's ever needed the recipe is recorded in §7.
- **MSI/MSI-X: kernel allocates the vector, userspace programs the
  device.** `KIRQ_MSI` returns an opaque `(address, data)` pair; the
  driver writes it into the device's MSI capability / MSI-X table
  itself (Redox's split, not Managarm's in-kernel PCI). The kernel
  never grows a PCI layer; whoever can already reach a device's
  config space and BARs is already trusted with its DMA engine, so
  self-programming adds no new authority (§5).
- **Claim policy is deferred.** IRQ rings are temporarily unrestricted.
  The intended end state is capability delegation through a devmgr; the
  kernel has no uid/account policy layer.
- **Affinity is deferred: everything routes to the BSP.** Also makes
  handler-side lock contention structurally nil in v1. A claim-time
  affinity argument slots in later without ABI change (the CQE and
  route carry no CPU assumptions).

## 1. Precedents

Surveyed from the three reference trees (`references/{minix,managarm,
redoxos}`); the scheme below is a synthesis, not an invention.

- **MINIX 3** (`kernel/interrupt.c`, `system/do_irqctl.c`): drivers
  register hooks via a kernel call checked against a per-service
  allow-list; the handler masks the line immediately, sets one active
  bit per hook, and converts the IRQ to a coalescing *notification*
  (a pending bitmap + non-blocking message). The line unmasks only
  when every hook has acked (`sys_irqenable` after device quiesce),
  unless the hook opted into `IRQ_REENABLE` auto-ack. Contributions
  taken: mask-until-ack as the level-line discipline, the policy
  table, mask-and-log for spurious lines, mask-on-owner-death.
- **Managarm** (`thor/generic/irq.cpp`): userspace holds an IrqObject
  handle, loops `awaitEvent(handle, seq)` → service → `helAcknowledgeIrq
  (ACK|NACK, seq)`. Every service increments a sequence; an await with
  a stale sequence completes immediately (no missed edge between
  service and re-arm), an ack with a stale sequence is rejected (no
  acking what you haven't seen). Level lines mask in service; edge
  lines buffer one raise. Shared lines: ack-wins counting, all-nack
  stalls the line with exponential-backoff auto-unstall. Contributions
  taken: the sequence-number race analysis (§3), trigger-mode strategy
  flags, the shared-line recipe (§7, deferred).
- **Redox** (`kernel/src/scheme/irq.rs`): the `irq:` scheme. Kernel
  keeps a per-line counter bumped in the handler; `read` returns the
  running count, `write(count)` is the ack + unmask and is ignored if
  the count moved meanwhile (the driver re-reads and services again).
  Legacy lines mask-until-ack; extended vectors (MSI) just EOI and
  count. Vectors are exclusively reserved via `O_CREAT`; userspace
  programs MSI address/data into the device itself; root-only.
  Contributions taken: nearly everything — the count-as-sequence, the
  ack-mismatch-refires semantics, the exclusive-vector stance, the
  MSI division of labor, excluding its uid-based policy. Redox's shape is close to
  isomorphic to a govindos scheme already.

The three agree on the core: **mask level lines until the driver says
the device is quiet; never lose an edge that races the ack; interrupts
coalesce, they don't queue.** They differ in transport (message vs
handle vs fd), which govindos already standardized as krings.

## 2. Scheme `-4`: irq

Many rings per process allowed, many claims per ring (a driver with
several devices, or one device's several MSI-X vectors, multiplexes one
ring — the `cookie` tells events apart). Ring creation is the ordinary
`SYS_VM_SHARE(base, -4, prot)`; capability gating is deferred (§0).

`abi/gdos/kring_irq.h`:

| SQE | fields | effect |
|---|---|---|
| `KIRQ_CLAIM` | a = gsi, b = cookie | exclusive claim of a GSI for this ring. `SYSERR_EXIST` (claimed anywhere), `SYSERR_INVAL` (no such GSI), `SYSERR_PERM`, `SYSERR_NOMEM` (claim-count bound, below). On success the line is programmed (trigger/polarity from ACPI) and unmasked |
| `KIRQ_RELEASE` | a = gsi | unclaim: masks the line, frees the route. `SYSERR_INVAL` if not this ring's claim |
| `KIRQ_ACK` | a = gsi, b = seq | "the device is serviced through `seq`": sets `acked = seq`, unmasks a level line, re-fires the event if raises slipped past `seq` (§3) |
| `KIRQ_MSI` | a = cookie | (phase 2) allocate an MSI vector: completion carries the opaque address/data pair in `a`/`b` (§5). Released by `KIRQ_RELEASE` on the returned pseudo-gsi |

| CQE | fields | when |
|---|---|---|
| `KEV_IRQ` | a = cookie, b = seq | the claimed line interrupted; `seq` is the raise count at post time. At most one un-consumed `KEV_IRQ` per claim — further raises coalesce into the count and re-fire after consumption + ack |

Event bound: at most one outstanding `KEV_IRQ` per claim, so
`nclaims + sq-completions` can never overflow the CQ; `KIRQ_CLAIM`
enforces `2·(nclaims+1) <= nslots`, the same accounting as wait-group
`ADD` (headroom for one event per claim plus the 1:1 completions).

The driver loop, entirely existing vocabulary:

```
setup:   VM_ALLOC -> ring; VM_SHARE(ring, -4)
         submit KIRQ_CLAIM{gsi, cookie}; DOORBELL; check completion
loop:    BLOCK_WAIT(&hdr->cq_count, seen)      // or a wait-group registration
         consume KEV_IRQ{cookie, seq}; advance cq_head
         ... read device ISR, drain queues, quiesce ...
         submit KIRQ_ACK{gsi, seq}; DOORBELL   // doorbell = consumption ack too
```

A driver process is otherwise ordinary: the `-4` ring composes with
`-1`/`-2`/`-3` and user channels, so one wait-group hears device
interrupts and client requests in the same park.

## 3. The count/ack protocol

Per claim (route entry, §4) the kernel keeps:

- `raised` — u64, incremented by the handler on every interrupt.
- `acked` — u64, set by `KIRQ_ACK` to its `seq` argument.
- `posted` — the outstanding-event bit, plus the CQ index of the
  outstanding `KEV_IRQ`; retired when consumption passes it
  (`index < cq_head`, FIFO — the wait-group `pending`/`ev_index`
  mechanism verbatim).

Rules:

- **Handler:** `raised++`. If `!posted`, post `KEV_IRQ{cookie,
  raised}` and set `posted` (CQ full ⇒ don't post — `raised != acked`
  *is* the pending level state, replay will deliver it). If `posted`,
  nothing: the raise is absorbed into the count.
- **`KIRQ_ACK(gsi, seq)`:** `acked = seq`; unmask if level-triggered.
  Then, if the outstanding event is retired and `raised != acked`,
  post `KEV_IRQ{cookie, raised}` again.
- **Doorbell replay (`ring_replay`, scheme `-4` arm):** for every
  claim with the event retired (or never posted) and `raised !=
  acked`, post. This is the consumption-ack replay every scheme has;
  it is what delivers the events a full CQ deferred.

Why this is lossless (Managarm's missed-IRQ race, restated): the
dangerous window is *driver consumed the event → serviced the device →
about to ack*, with an edge firing inside it. The edge bumps `raised`
while `posted` still covers it, so nothing new posts; the driver's ack
then carries the pre-edge `seq`, the kernel sees `raised != acked`,
and re-fires with the new count. The driver services again and acks
the new seq. No edge is dropped, no unconditional re-fire storm, and
nothing is ever rejected — a stale ack is simply an ack that doesn't
quiesce the pending state. On a masked level line the race is dead by
construction (`raised` can't advance while masked), so ack-and-unmask
always finds `raised == acked` there; the re-fire path exists for
edge/MSI, where nothing is ever masked.

Masking policy per trigger mode (kernel-chosen from ACPI; the claimant
has no say):

- **Level GSI:** handler masks the IOAPIC RTE, EOIs the LAPIC, posts.
  `KIRQ_ACK` unmasks. Mask-until-ack is what makes a still-asserted
  line survivable with the handler done and the driver not yet run.
- **Edge GSI / MSI:** never masked; EOI + count. Coalescing plus the
  ack re-fire carry the whole burden.
- **Unclaimed vector fires** (late device, driver died, misprogrammed
  MSI): mask if it has an RTE, count it, log once. MINIX's spurious
  policy.
- EOI is always the handler's, before iret — the LAPIC is per-CPU
  state and userspace never touches it. The RTE mask is the only
  driver-visible latch.

## 4. Delivery: posting from the IRQ handler

The context analysis this rests on (verified against the
implementation, not just the design docs): device interrupts arrive
only in IF=1 contexts — ring 3, or the scheduler loop's idle/dispatch
windows — because `spinlock_lock` is cli-first and the syscall path
runs entirely under IA32_FMASK's IF mask. So the handler enters
holding nothing, and every spinlock holder machine-wide is IRQs-off
and bounded: the handler may take stripes and the scheduler lock (the
preempt path already does the latter). The one forbidden lock is
`g_umem` — it is a shootdown-servicing svclock, and a handler spinning
IRQs-off on it can be the shootdown target its holder waits on.

The stripe plane suffices, because a `KEV_IRQ` post is the same act as
a peer's data-plane doorbell:

- `ring_post_locked` needs stripe(ring block) only.
- If the ring's owner is parked, the post wakes the slot
  (`thread_unblock`: scheduler lock, legal here).
- If the ring is registered in a wait-group, delivery is
  `groups_notify_locked` under stripe(group block) — the g_umem-free
  data-plane entry that already exists for exactly this, using the
  ascending-stripe ranked acquisition of `wake_user_side_ranked`
  (release, reacquire in index order, revalidate the slot). The
  handler-side post reuses that shape (factored helper, not a copy).

**Route table and pinning.** Static `irq_route[]`, one entry per GSI
plus the MSI vector range: `{ leaf spinlock; struct ring *ring; u64
cookie; u64 raised, acked; posted + ev_index; mode; masked }`. The
handler resolves vector → route, takes the route lock, reads
`route->ring`, and holds the route lock across the whole post: that
pin is what keeps the ring and its block alive without `g_umem`,
exactly the way a slot observation pins a reg today. Lock order:
**route lock < stripes** everywhere (handler: route → stripe(s);
control plane: g_umem → route → stripes). Wake-state (`raised`,
`posted`) lives under the route lock — the route is its stripe-analog;
the CQ bytes themselves are published under the ring's stripe as for
every scheme.

**Teardown.** The ring keeps its claim list (g_umem-only state, like
`regs`). `channel_block_torn` gains a `-4` arm: for each claim, take
the route lock, **mask the line**, clear `route->ring`, release — then
the normal endpoint teardown. After the route lock cycles, no handler
can reach the ring, so the free is safe; and a dead driver never
leaves a screaming level line unmasked (MINIX's `rm_irq_handler`
rule). `KIRQ_RELEASE` is the same sequence minus the endpoint
teardown. Process death needs nothing new: the ring block is owned by
the driver, so the zombie holds it and the parent's reap revokes it
through this same path.

**No faults in IRQ context.** CQE stores go through the SASOS identity
map from any CR3 (the ipc doc's IOCP-style posting), but a kernel-mode
#PF inside a handler is a panic, so the store must be infallible. Two
rules make it so: (1) `SYS_VM_PROTECT` is rejected on a block with
`b->ring != nullptr` — new rule, worth adopting for all kernel
channels regardless (control-plane posts from foreign syscall contexts
have the same latent hazard; kernel channels already refuse `VM_MOVE`
for the same owner-bound-identity reason); (2) the revoke path already
runs `channel_block_torn` — which now silences the routes — strictly
before any PTE change, so a handler post can never race the unmap.

Handler skeleton (the `default:` branch `interrupt_handler` grows for
the device-vector range):

```
irq_deliver(vector):
  route = &irq_route[vector - VECTOR_DEVICE_BASE]
  lock(route)                    // leaf; pins route->ring
  if route->ring == null: mask RTE if any, count, unlock, EOI, return
  if route->mode == LEVEL: ioapic_mask(route)
  route->raised++
  if !route->posted:
    stripe-post KEV_IRQ{cookie, raised}; ranked group delivery
    route->posted = true on success   // CQ full: leave for replay
  unlock(route)
  x86_lapic_eoi()
```

Latency note: the post makes the driver runnable; when it runs is the
scheduler's business, and drivers run at normal priority. Interrupt
latency therefore includes up to a quantum of someone else — a
priority story is future scheduler work, not IRQ-scheme work (MINIX
gives drivers high static priority; noted, deferred).

## 5. MSI / MSI-X

What MSI is, in one paragraph: instead of a shared physical wire into
the IOAPIC, the device *DMA-writes a data word to a magic address*
(x86: the `0xFEE0_0000` window, routed to LAPICs — the address encodes
the target CPU, the data encodes the vector). Consequences: per-vector
= never shared (no nack machinery), inherently edge (nothing to mask),
ordered with the device's data writes (no read-back flush), and the
IOAPIC is not involved — the kernel's whole job is picking vector and
CPU. MSI-X is the modern variant: up to 2048 independent
`{addr, data, mask}` entries in a table in one of the device's own
BARs (plain MMIO), vs MSI's single pair in PCI config space.

Division of labor (Redox's, deliberately not Managarm's — govindos has
no kernel PCI layer and refuses to grow one):

- **Kernel (`KIRQ_MSI`):** allocate a free device vector, bind it to
  the ring/cookie as a route with no RTE, return the `(address,
  data)` pair in the completion CQE. The pair is **opaque to the
  driver** — the kernel composes it (destination, delivery mode,
  vector; someday the IOMMU-remapping format), which keeps affinity
  and hardening kernel-owned without ABI change. Vector choice is the
  security-critical half and stays kernel-only.
- **Driver:** writes the pair into the device — MSI capability in
  config space, or the mapped MSI-X table BAR — and sets the enable
  bit. From then on: device write → LAPIC → `irq_deliver` → `KEV_IRQ`,
  the same path as a pin interrupt, with edge/count semantics.

Prerequisite honesty: this presupposes the driver can *reach* config
space and BARs, which is the missing device-memory story (map the
ECAM window / BAR ranges into driver processes — a future pcid owning
enumeration and handing each driver its function's pages), and a trust
decision: without an IOMMU, config-space write access is already
full-machine trust (the device's normal DMA engine is right there), so
MSI self-programming adds nothing new; with an IOMMU, interrupt
remapping filters hostile pairs — future kernel work behind the same
`KIRQ_MSI` op. Neither blocks the pin-IRQ scheme, which is why MSI is
phase 2.

## 6. Hardware prerequisites (kernel-side, all bounded)

- **IOAPIC driver** (`archsrc/x86_64/ioapic.c`): MADT parse for IOAPIC
  bases + GSI ranges + ISA source overrides (trigger/polarity); RTE
  program/mask/unmask behind a leaf spinlock (the index/data register
  window is racy by design). Today only the MMIO page gets mapped
  (`cpu_setup.c`).
- **Device vector allocator:** a fixed range (`0x30..0xEF`), avoiding
  the existing special vectors (TLB shootdown, resched, preempt) and
  the exception block. Static array, no dynamism.
- **`interrupt_handler` branch:** device-range vectors dispatch to
  `irq_deliver` instead of `fault_panic`.
- **Route table:** sized `max GSI + MSI range`, static.

GSI discovery is out of scope: a driver learns its device's GSI from
ACPI (`_PRT` for PCI INTx) or platform knowledge — userspace's problem
(the future pcid/acpid), like everything else about enumeration.

## 7. Deferred, with recipes

- **Shared legacy lines.** If ever needed: per-claim ack bits on one
  route (MINIX `irq_actids`), unmask on all-ack; add a nack op so a
  driver can say "not mine" cheaply, ack-wins resolution, and
  all-nack ⇒ mask + exponential-backoff unstall kick (Managarm
  `maskedForNack`, exponent capped). The ABI holds: `KIRQ_CLAIM`
  grows a SHARED flag, `KIRQ_NACK` joins `KIRQ_ACK`. Not before a
  real device forces it — PCIe devices are MSI-capable by spec.
- **Claim policy.** A future devmgr owns the allow-list (MINIX's RS +
  system.conf as the model), likely as
  the same process that owns ECAM and hands out device memory — claims
  then become something it brokers, not something the kernel
  policy-checks per-GSI.
- **Affinity + per-CPU routing.** A claim/`KIRQ_MSI` argument choosing
  the destination CPU; route locks already make concurrent handlers on
  different CPUs safe. Until then: BSP.
- **Driver priority.** Scheduler feature; see §4 latency note.
- **Kernlet-style in-kernel ack automation** (Managarm's
  `helAutomateIrq`): rejected for now — it is a bytecode interpreter
  in the kernel for a latency win no current driver needs, and it
  cuts against "the kernel never learns the protocol".

## 8. Suggested implementation order

1. **IOAPIC + vector plumbing** (§6): MADT/RTE driver, vector
   allocator, `irq_deliver` skeleton that masks-and-logs everything
   (the spurious policy is the whole handler at this stage). Testable
   bare: unmask a line, see the log.
2. **The `-4` scheme, pin IRQs** (§2–§4): route table, claims,
   count/ack, handler post (route-lock pinning + factored ranked
   wake), `channel_block_torn` arm, `ring_replay` arm, the
   `VM_PROTECT`-on-ring-blocks rejection. First consumer: the PIT or
   RTC on a legacy line, or a QEMU serial/e1000e pin interrupt —
   something tests.c can claim and count deterministically under QEMU.
3. **Wait-group composition test**: one group hearing `KEV_IRQ` +
   a user channel — the canonical driver main loop.
4. **MSI** (§5): `KIRQ_MSI`, the vector-only route arm — once the
   device-memory story (ECAM/BAR mapping) exists to make a real MSI
   driver possible.
