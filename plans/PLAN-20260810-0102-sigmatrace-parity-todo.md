# PLAN — parity with SigmaTrace on an IBGDA run

- **Created:** 2026-08-10 01:02 +03
- **Status:** TODO — designed; step 3's assertion written, nothing run yet.

## du04 access, since it is not obvious

`ssh a100` is configured with `ProxyJump university-base`, and **that bare name
does not resolve** — MagicDNS is not answering on simurgh-base. The jump has to
be spelled with the tailscale address:

```bash
ssh -J mktips@100.64.0.8 missa18@172.20.31.90     # university-base -> du04
```

Confirmed 2026-08-10: `du04`, 2x A100-SXM4-40GB, **`PeerMappingOverride=1`** —
the capability whose absence makes all four IBGDA cases SKIP on MN5.

`~/proj/pcomm` on du04 is a **partial, pre-suite deployment** (no `tests/`, no
git). It needs the current tree before any of this runs; that sync is the first
practical task, not part of the design.
- **Related:** `PLAN-20260803-2253-ibgda-device-initiated-capture-in-progress.md`
  (the doorbell hijack this validates); `tests/hpc/cases/12_ibgda_doorbell`
  (the capture side already exists).

## The question

Does pcomm see every event SigmaTrace sees, on a GPU-initiated run? SigmaTrace
is the predecessor and the only independent implementation, so it is the only
external check available on the DEVX path.

**Timings are not comparable and do not need to be.** Both tools record a
doorbell's `prod_index` — SigmaTrace in `DoorbellRingData/Control`, pcomm in
`Mlx5Doorbell.prod_idx` and `Mlx5DeviceSend.wqe_idx`. That is a **per-QP
monotonic sequence number, not a clock**. A doorbell at index N on a given queue
pair is the same logical event in every run of the same workload, whenever it
happened. The whole comparison can therefore be built on sequence identity and
never touch a timestamp.

## Two runs, not one

Both tools are `LD_PRELOAD` interposers on the same `mlx5dv_*` entry points, and
pcomm's own `smart_dlsym` throws on a duplicate symbol registration. Co-residency
is a research question, not a test fixture. So: **two separate runs, same binary,
same node, same parameters** — and the cost of that choice is that run-to-run
variation becomes a confounder, which step 1 exists to bound.

Same binary matters. Case 12 builds `nvshmem_putget_sm80` from
`tests/hpc/cases/04_nvshmem/putget.cu` with fixed `ELEMS`/`ITERS`; SigmaTrace has
its own apps under `Apps/`, and using them would make the workload a second
variable. Build once, run under each tool.

## MEASURED 2026-08-10 — step 1 ran, and it inverted the design

Two pcomm runs of case 12 on du04, identical parameters, canonical fingerprint:

```
                 run A     run B
device_sends      1309      1309     identical
opcode histogram  same      same     atomic_masked_cs 59, rdma_write 1250
total_bytes     160000    160000     identical
wqe_bag       109d403a  109d403a     IDENTICAL
umem_bag      d9396ba8  d9396ba8     identical
doorbells          168       162     DIFFER
doorbell_bag  9dd22446  c7e18a66     DIFFER
```

**The work is deterministic; the notifications are not.** The same 1309 WQEs —
same slots, same opcodes, same lengths — were announced by a different number of
doorbells. NVSHMEM batches warp-combined writes and rings once per batch, and
where a batch ends moves with timing. The risk this plan listed last ("each tool
perturbs timing, which can change how many doorbells ring") is not a risk, it is
the measured behaviour, and it shows up between two runs of the *same* tool.

**This inverts what the plan said above.** "Doorbell identity and sequence is the
parity claim" is wrong: the doorbell count is the one thing that does not
reproduce, so it cannot carry a claim. The decoded WQE stream is the stable
surface.

And that creates the tension to design around: the surface that *is* repeatable
(the WQEs) is exactly where SigmaTrace is expected to disagree, because of the
AV-segment layout difference below. So the comparison may lean on neither
doorbell counts (unstable) nor decoded fields taken at face value (contested).

**What survives as a sound parity claim**, all three measured deterministic
here: WQE count, total bytes, and the opcode histogram. Plus the set of
announced slots — with doorbell counts compared only as "both tools saw rings
covering the same work", never as equality.

Corollary for the fingerprint in step 2: build it on the per-QP **WQE**
sequence, not the per-QP doorbell sequence. `wqe_bag` was byte-identical across
runs; `doorbell_bag` was not.

Both runs passed all 14 case-12 assertions, which also means the
trailing-doorbell rule in `doorbell_sequence_complete` holds across differently
batched runs rather than only the one it was derived from.

## Step 1 — self-repeatability, before any cross-tool claim

**Run pcomm twice and require the canonical fingerprint (below) to be
byte-identical.** Only then is a pcomm-vs-SigmaTrace difference attributable to
the tools rather than to the run.

This is the gate the suite already applies to goldens — "a golden is only
committed after the same query yields byte-identical output across two
independent runs" — and it matters more here, because a cross-tool mismatch with
no repeatability baseline is uninterpretable in both directions.

Do the same for SigmaTrace. If either tool is not self-repeatable on this
workload, the parity test as designed cannot run, and the finding is that
instead.

## Step 2 — the canonical fingerprint

Everything run-specific is projected away. What varies between runs and must not
appear: timestamps, virtual addresses, `qp_num`, `mem_id`, UAR/context pointers,
`wr_id`. What is stable and carries the meaning: sequence indices, opcodes,
lengths, sizes, and counts.

**Doorbells — the primary surface.** Per queue pair, build the ordered sequence
of `(prod_idx, opcode, length)`, then compare the **multiset of per-QP
sequences** across tools. Keying on a QP ordinal was rejected: it assumes both
runs create queue pairs in the same order, which nothing guarantees. A bag of
sequences is invariant to both QP naming and QP ordering, and still detects a
single missing or extra doorbell anywhere.

**Registrations and UARs.** Multiset of `(size, access)` for umem; counts and
`flags` multiset for UARs; open/close counts must balance per class.

**Scalars, as a fast triage line:** doorbell count, distinct QP count, opcode
histogram, total bytes.

## Step 3 — `prod_idx` contiguity, on each capture independently

Per `(rank, qp_num)`, `prod_idx` must form a contiguous run. A gap is a doorbell
that tool missed.

This needs no second tool at all, it is strictly stronger than a count
comparison because it *localises* the miss, and it should be an assertion in
case 12 regardless of whether the SigmaTrace comparison ever runs. **Build this
first** — it is most of the value for a fraction of the work.

## Step 4 — compare, and report a coverage matrix rather than a verdict

The assertion is **directional, not equality**: for every event class SigmaTrace
records, pcomm must record at least the same multiset. Extra records on our side
are a superset and expected — we emit `Mlx5DevxQp`, `Mlx5DevxQpConnect` and
`Mlx5DevxCmd`, which have no SigmaTrace counterpart, and SigmaTrace splits
doorbells into `Data`/`Control` where we carry one type distinguished by opcode.

Output is a table with one row per event class and four columns: *both*,
*pcomm only*, *SigmaTrace only*, *disagreed*. A single PASS/FAIL would discard
exactly the information the test exists to produce.

The mapping, read from `Profiler/Events/Mlx5DvEvents.hpp`:

| SigmaTrace | pcomm |
|---|---|
| `Mlx5dvMemoryRegistration` | `Mlx5Umem` (200) |
| `Mlx5dvMemoryDereg` | `EntityClose` opener=200 |
| `Mlx5dvUarAlloc` | `Mlx5Uar` (202) |
| `Mlx5dvUarFree` | `EntityClose` opener=202 |
| `DoorbellRingData` | `Mlx5Doorbell` (212) + `Mlx5DeviceSend` (214), data opcodes |
| `DoorbellRingControl` | `Mlx5DeviceSend`, non-data opcodes |

## **SigmaTrace is not ground truth for decoded fields**

This is the most important constraint in the plan and it is already established
by measurement, not suspected.

`things-to-circle-back-to` and the IBGDA plan record that SigmaTrace's WQE
decoder **assumes a full 64-byte AV segment and pointer data segments**, while
NVSHMEM 3.3.24 on du04 emits a **half AV (`ds == 4`) and inlines warp-combined
payloads** (`MLX5_INLINE_SEG`). pcomm's decoder was written from the NVSHMEM
source for exactly that reason.

So the two tools are expected to **agree on which doorbells rang and disagree on
what the WQEs said**. That is not a bug to chase during this test:

- **Doorbell identity and sequence is the parity claim.** `prod_idx` comes off
  the doorbell itself, needs no WQE parsing, and is where agreement is
  meaningful.
- **`opcode` and `length` are the diagnostic, not the claim.** A disagreement
  there localises to WQE layout, and case 12's data-plane `VERIFY OK` is
  independent evidence for our reading.

Design the comparison so a decode disagreement cannot fail the doorbell-identity
check. If they are conflated, the test will report "no parity" over a difference
we already understand and have already resolved in our favour.

## Step 5 — reading SigmaTrace's logs

SigmaTrace's `ProfilerEventReader` produces a comm-graph JSON and a Perfetto-ish
trace — neither is a raw event dump. Its `Events/EventReader.hpp` is the reading
primitive, and `Events/Tests/reader_visitor.cpp` is a worked example.

Write a small dumper against that header emitting the same fingerprint TSV
pcomm's side emits, so the comparison is TSV-vs-TSV and neither tool's analysis
code is in the loop. Do not compare via the comm-graph JSON: it is a derived,
opinionated view, and a difference in it would not distinguish "missed an event"
from "aggregated it differently".

## Risks

- **The workload may not be deterministic enough**, in which case step 1 fails
  and this plan stops there. That is a real possible outcome and a publishable
  one; it must not be worked around by loosening the fingerprint until the test
  passes.
- **du04 is the only host.** All four IBGDA cases SKIP on MN5 for want of
  `PeerMappingOverride=1` (confirmed on the 2026-08-10 board, job 44451619), so
  none of this is reachable from the cluster the rest of the suite runs on.
- **Building SigmaTrace** is unscoped here and may be the largest single cost.
- **Interposer interference**: even in separate runs, each tool perturbs timing
  (the hijack costs ~+68% wall), which can change how NVSHMEM batches warp
  writes and therefore *how many doorbells ring*. If doorbell counts differ but
  each is internally contiguous, suspect this before suspecting a missed event —
  it is a difference in the observed program, not in the observation.

## Explicitly out of scope

- Running both tools in one process.
- Timing comparison of any kind.
- Parity on the verbs path — the ib_* records have their own suite coverage, and
  the DEVX path is where we have no independent check.
- Fixing SigmaTrace's WQE decoder.
