# PLAN — the unclosed regions at scale are hcoll's, and they are a real leak

- **Created:** 2026-08-12 02:50 +02
- **Status:** UNDERSTOOD AND CLOSED. Not a pcomm defect. The capture is correct,
  the assertion is correct, and the fix is to say so in the one place it fires.
- **Reproducer:** `tests/hpc/probes/leaderdereg/job.sbatch` (parametric in
  `--nodes`), kept rather than deleted — it is the only thing that can re-answer
  this when HPC-X changes.
- **Related:** `PLAN-20260805-0053-entity-lifetimes` (the assertion that found
  it); `PLAN-20260806-2320-dlvsym-interposer-not-observed` (the caller-trace
  diagnostic this reused).

## What fired

The 64-rank scale capture (job 44514759, 16 nodes x 4, `-p ib`) is green on
everything structural — 126-edge star exact, **zero unresolved peers, zero
self-edges across 16 LIDs** — and red on `entity_close_accounted` clause 3, with
a signature too regular to be a race:

```
32 unclosed ib_mem_region = exactly 2 on each of the 16 ranks with rank % 4 == 0
                            (local rank 0 of each node), 0 on the other 48
per leader: one 40-byte and one 8388608-byte host region,
            same PD, access=3 (LOCAL_WRITE|REMOTE_WRITE), origin=reg_mr
```

## Three explanations, and none of them could be reasoned away

1. the owning library registers these and never frees them;
2. it frees them through a door we do not hook;
3. it frees them after pcomm has finalized.

## What was measured

**Job 44516867 — 2 nodes x 4, A: `NVSHMEM_DISABLE_P2P=1`, B: control.**
Both sections: **0 unclosed**, 88 caller lines each (so the diagnostic ran; the
zero is a real zero). This killed the hypothesis the probe was built on —
`DISABLE_P2P` has nothing to do with it — and, because the probe had `-np 8`
hardcoded while being the tool meant to bisect width, it also showed the
instrument was wrong. `-np` now comes from `SLURM_NNODES`.

**Job 44517372 — 8 nodes x 4, same two sections.** Reproduced in **both**:
2 unclosed on each of ranks 0,4,8,...,28, the same 40 and 8388608 bytes. So the
variable is **node count**, not the flag and not the provider selection.

**The owner, named.** `PCOMM_TRACE_MR_CALLERS` keyed on `(door, library)` cannot
say which library registered a *particular* region, and `reg_mr` has two callers
here. Keyed on `(door, library, length)` it does, and the answer is unambiguous:

```
hmca_mcast_vmc.so   reg_mr len=20971520     dereg_mr len=20971520    balanced
hmca_mcast_vmc.so   reg_mr len=8388608      (no dereg, ever)         LEAKED
hmca_mcast_vmc.so   reg_mr len=40           (no dereg, ever)         LEAKED
```

`hmca_mcast_vmc.so` is HPC-X **hcoll's multicast component**. It registers three
regions and deregisters one.

## Which reading that is, and why it is not the other two

**It is (1).** Explanation (2) is excluded because we see every `ibv_dereg_mr`
this library makes — the 20971520 region's is right there — so its frees are not
going through some unhooked door; there simply are no frees for the other two.
Explanation (3) is excluded because the caller trace prints from the interposer
independent of capture state: a deregistration after finalize would still have
printed a `dereg_mr ... len=40` line, and none exists. And the leak is per
NODE LEADER because that is where hcoll puts the multicast group's buffers.

So: a third-party collective library allocates two per-node buffers and lets
process exit reclaim them. That is legal, harmless, and not ours. It is also
exactly the second reading `entity_close_accounted` names in its own comment —
"an application that genuinely exits without cleaning up, which is legal and not
our problem" — written before this happened.

## Measured points (do not re-derive)

| shape | job | hcoll leak |
|---|---|---|
| 4 nodes x 1 rank (cases 16/17/18) | 445025xx | none — suite is green |
| 2 nodes x 4 ranks | 44516867 | none |
| 4 nodes x 4 ranks | 44517375 | none |
| 8 nodes x 4 ranks | 44517372 | 2 per node leader |
| 16 nodes x 4 ranks (scale job) | 44514759 | 2 per node leader |

**The threshold is between 4 and 8 nodes**, which is hcoll's multicast enabling
itself by node count — a property of HPC-X's configuration on MN5, not of
anything pcomm does. Both `DISABLE_P2P` settings were measured at every point
below 8 and it never mattered.

The consequence that decides the scope of the fix: **no suite case can reach
this.** The largest is 4 nodes. Only the corpus scale job crosses the threshold,
so the exemption is one case wide by construction rather than by choice, and the
board keeps its full meaning.

## The fix

`expect_fail_asserts` on `entity_close_accounted` in the scale job's meta.json,
with this plan named in the reason. That is the codebase's own prescription for
this reading ("scope the assertion to the cases where cleanup is part of the
workload rather than deleting it"), and `sql_check.sh` reports **XPASS** if it
ever starts passing — so the day HPC-X fixes this, the suite says so instead of
staying quietly green on a stale marker.

**Deliberately NOT done:** exempting opener 100 from clause 3 globally. Two of
the three prior investigations in this area were about unclosed/unmatched memory
regions being a real defect (the CQ third door, the 474 unmatched closes). A
global exemption would have hidden both. The exemption is one case wide.

## What would make this precise instead of scoped

The assertion cannot tell "hcoll's internal buffer, legally leaked" from "a
dereg door we do not hook" because **the capture does not record who registered
a region**. Recording it would turn every future unclosed-entity investigation
from a three-job bisect into one query.

The mechanism already exists in miniature: `dladdr(__builtin_return_address(0))`
at each MR door, currently gated behind `PCOMM_TRACE_MR_CALLERS` and printed to
stderr. Making it a recorded, interned `owner` column means a new record version
(`IbMemRegionV2`, next free id in the ibverbs range, `version 2`, with a
version_map upconversion) — the convention in `core/record.hpp`, because a
record's layout is immutable once a capture holds it. `origin` on this same
record was added for the same class of reason and is the precedent.

**Trigger to build it:** the next time an unclosed or unmatched entity costs a
cluster job to attribute. It has now cost three (44368870, 44514759, and this
bisect). One more and the column is cheaper than the investigations.
