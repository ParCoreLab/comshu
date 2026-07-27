# PLAN — fix the NVBit bounded-channel deadlock

- **Created:** 2026-07-27 10:22 +03
- **Status:** backlog
- **Unblocks:** `05_nccl_p2p_xfail`, `07_nvshmem_multinode_xfail` — and with 07,
  the unified NVLink+IB *traffic* timeline, which is the project's headline
  result and currently has no regression test at all.
- **Related:** `plans/PLAN-20260727-0126-shared-engine-abi-todo.md` (its
  Sequencing section notes this gap), `things-to-circle-back-to.md`

## The failure

Two independent workloads hang under the gpumem/NVBit provider:

| case | workload | topology | outcome |
|---|---|---|---|
| 05 | NCCL all-reduce | 1 node × 4 H100 | deadlock (rc 124) |
| 07 | NVSHMEM put-ring | 2 nodes × 4 H100 | deadlock (rc 124) |

Both are the same shape: **a comm library that spins on the device, versus
NVBit's bounded host↔device channel that needs the kernel to make progress in
order to drain.** NCCL's persistent collective kernel occupies the GPU
outright. Multinode NVSHMEM's IBRC path has device code waiting on a host proxy
thread. Either way the producer fills the channel, the consumer can't drain it,
and nobody advances.

Single-provider vs. co-run is **not** a factor — this was isolated 2026-07-27:

| topology | providers | result |
|---|---|---|
| 1 node, 4 PEs  | ib + p2p                | exit 0   |
| 2 nodes, 8 PEs | p2p **alone**           | exit 124 |
| 2 nodes, 8 PEs | p2p, `NO_EAGER_LOAD=0`  | exit 124 |

NVBit's own banner hint for deadlocks (`NO_EAGER_LOAD=0`) does not help.
Snoopie hits the same wall, so this is not a defect we introduced — it is the
known limit of the technique, and beating it is a genuine contribution.

## Candidate fixes (unevaluated — that is the first task)

1. **Spillable / host-pinned channel.** Remove the bound: when the ring fills,
   spill to host-pinned memory rather than blocking the producer. No device-side
   flush kernel needed, so a spinning kernel can't starve it. Highest chance of
   working; costs memory and ordering care.
2. **Cooperative-launch flush.** A flush that can preempt the persistent kernel.
   Needs cooperative launch support and probably conflicts with how NCCL
   launches its own kernels — investigate feasibility before investing.
3. **IBGDA transport (07 only).** GPU posts WQEs directly, no host proxy to wait
   on. But it also bypasses host verbs entirely, so the `ib` provider would see
   nothing — this fixes the hang at the cost of the very cross-transport capture
   case 07 exists for. Only useful as a diagnostic to confirm the proxy-wait
   theory, not as the fix.

## Suggested first step

Confirm the mechanism before building anything: instrument the channel to log
occupancy and blocked-producer counts, run case 05 (fastest repro, 1 node), and
verify it is a full-ring producer block rather than something else. Cheap, and
it decides between fixes 1 and 2.

Note case 07's repro is ~5 min on 2 nodes; case 05 is the tighter loop.

## Definition of done

`05_nccl_p2p_xfail` and `07_nvshmem_multinode_xfail` go **XPASS** — which
`run_suite check` already reports as an alert ("it started working, so remove
the marker and add goldens"). Then the xfail markers come off, goldens land via
the usual two-run determinism gate, and `07`'s `cross_transport.sql` finally
gets a golden — giving cross-transport traffic correlation a regression test.
