# PLAN — correctness checks derived from the workload, alongside the goldens

- **Created:** 2026-08-03 15:20 +03
- **Status:** DONE (2026-08-03) — plumbing, cross-cutting assertions and all
  eight cases landed; validated against the stored MN5 captures without any
  cluster time. Three derivations in this plan did **not** survive contact with
  the data; see "What the data changed" at the end.
- **Findings:** `things-to-circle-back-to.md` — "goldens that encode
  run-specific identifiers", "NVSHMEM heap attribution depends on when a record
  is drained".

## The problem with what we have

Every case is checked by a golden: a query's output diffed against a blob
captured earlier. That is a **characterization** test — "the code still does what
it did" — and it has two failure modes we hit three times in one evening
(2026-08-03):

1. **It cannot catch a bug that predates it.** `04_nvshmem`'s `peer_fanout`
   golden asserts `peers = 2`. The same file's comment says *"the ring put has
   each PE store to one neighbor"*. A one-neighbour ring cannot have fan-out 2 —
   the golden **enshrines** the NVSHMEM attribution bug, and the check is green.
   No golden can ever find that, because the golden *is* the defect.
2. **It fails on things that were never the claim.** `01`/`02` encode a
   particular `qp_num → rank` outcome; the fabric handed out `qp_num 449` to
   both ranks and they went red with nothing wrong in the tool. `07` encodes one
   drain interleaving and moves whenever timing does.

A golden also cannot explain itself. `2c2 < 0,0,20,81920 > 0,1,20,81920` says
what changed and nothing about what is *true*.

**Goldens keep a job** — pinning behaviour that genuinely cannot be derived,
like NCCL's internal channel and protocol choices. What changes is which one is
load-bearing: a case must have at least one **derived** assertion, and the
golden becomes the regression net behind it.

## The idea

Almost every number in these captures is *computable from the command line*.
The oracle is already in `job.sbatch`; it has simply never been written down as
one:

```
ibv_rc_pingpong -n 20          ⟹ 20 sends × 4096 B = 81920 B per side
p2p_kernel 4096 4  (4 GPUs)    ⟹ 4×4×4096 = 65536 stores; ring 0→1→2→3→0
putget 65536 3                 ⟹ 65536×3 = 196608 accesses per op; fan-out 1
ib_send_bw -n 1000 -s 8192     ⟹ 1000 sends × 8192 B = 8192000 B
```

Three of those four match today's goldens exactly, which is the encouraging
part: the goldens are mostly *right*, they are just not *justified*. The fourth
(fan-out) is where a derived check would have caught a real bug on day one.

## Mechanism

**`meta.json` gains a `workload` block** — the oracle's inputs, in one
machine-readable place instead of scattered through shell variables:

```json
"workload": { "ranks": 2, "nodes": 2, "gpus_per_node": 4,
              "iters": 20, "size": 4096, "elems": null }
```

**`sql_check.sh` materialises it as a `params` table**, so an assertion states
its own derivation in SQL:

```sql
-- asserts/traffic.sql — every rank posts exactly iters sends of size bytes
SELECT rank, count(*) AS n, sum(CAST(length AS INTEGER)) AS bytes
FROM ib_post_send GROUP BY rank
HAVING n     != (SELECT iters FROM params)
    OR bytes != (SELECT iters * size FROM params);
```

**An assertion query returns the offending rows; empty means pass.** No golden
file, the query is the specification, and a failure prints exactly what violated
it rather than a diff of two opaque blobs.

Layout: `checks/` keeps the goldens as they are; a new `asserts/` holds the
derived checks. The runner reports them separately so a red board says which
kind failed — a failed assertion is a correctness bug, a failed golden is a
change that may or may not be one.

## Every case, and what is actually deducible

`D` = derived from workload parameters. `I` = invariant (true regardless of
run-specific values). `G` = must stay a golden, genuinely not derivable.

### 01_ib_pingpong — `ibv_rc_pingpong -n 20` (4096 B default), 2 ranks, 2 nodes

Everything about this case is deducible; it is the best starting point.

| assert | kind | statement |
|---|---|---|
| `traffic` | D | each rank posts exactly `iters` sends totalling `iters*size` bytes |
| `regions` | D | exactly 1 memory region per rank |
| `completions` | D+I | `iters` send + `iters` recv completions per rank, **all status success** |
| `topology` | I | `comm_edges` is a permutation: each rank has exactly one peer, and it is not itself |

The last one is the fix for tonight's `qp_num 449` failure. *Which* rank is the
peer is a fabric detail; **that there is exactly one, and it is someone else**,
is the claim. Today's golden names it and breaks when the fabric reuses a QP
number.

### 02_nccl_internode — `nccl_allreduce 8388608 20`, 8 ranks, 2 nodes

NCCL chooses channels, protocol and chunking; byte counts are not derivable
(pinning already cut sends 57280 → 26880). Assert shape, keep volume golden.

| assert | kind | statement |
|---|---|---|
| `completions_healthy` | I | every completion is `success` — zero `err*` rows |
| `all_ranks_participate` | D | exactly `ranks` distinct ranks appear, none with zero sends |
| `degree` | I | every rank's peer count is equal (symmetric collective), > 0 |
| `sends_have_payload` | I | no send has `length = 0` |
| traffic volume | G | keep — NCCL-determined |

### 03_p2p_kernel — `p2p_kernel 4096 4`, 4 GPUs, 1 node

Fully deducible, and the reference for what correct attribution looks like.

| assert | kind | statement |
|---|---|---|
| `traffic` | D | stores = `gpus * iters * elems` = 65536, bytes = `* 4` = 262144 |
| `peer_topology` | D | ring: `mem_dev = (run_dev + 1) % gpus`, each edge exactly `iters*elems` = 16384 |
| `objects` | D | exactly `gpus` allocations |
| `owner_in_range` | I | every `mem_dev` < `gpus` |

`owner_in_range` is trivial here and is the point: **the same assertion on case
04/06/07 would have caught `mem_dev = 64` instantly and loudly**, instead of it
surfacing as a doubled count three weeks later.

### 04_nvshmem — `putget 65536 3`, 4 PEs, 1 node

Where a derived check finds a live bug.

| assert | kind | statement |
|---|---|---|
| `traffic` | D | per rank per op: `elems*iters` = 196608, bytes `*4` = 786432 |
| `peer_fanout` | D | **exactly 1** peer per GPU — a ring put has one neighbour |
| `owner_in_range` | I | every `mem_dev` < `n_pes` |
| `objects` (34/rank) | G | keep — NVSHMEM's internal allocations |

**`peer_fanout` is expected to FAIL when written**, against a golden that
currently passes at 2. That is the plan working as intended: it is blocked on
`PLAN-20260803-1448-nvshmem-heap-attribution-todo.md`. Land the assertion
`expect_fail`-marked, and let fixing attribution flip it green — the assertion
becomes the acceptance test for that plan.

### 05_nccl_p2p — `nccl_p2p 262144 3`, 4 PEs, 1 node

Volume is genuinely unassertable: NCCL's LL protocol spins on flag polling —
295,044 vs 550,064 loads across identical runs. Structure only.

| assert | kind | statement |
|---|---|---|
| `ops` | I | every rank records both a load and a store |
| `all_ranks_participate` | D | exactly `ranks` distinct ranks, all non-empty |
| `owner_in_range` | I | every `mem_dev` < `gpus` |
| peer_traffic | G | keep |

### 06_nvshmem_coexist — `putget 65536 3`, 4 PEs, `-p ib,p2p`

The case exists to prove two providers co-capture in one process.

| assert | kind | statement |
|---|---|---|
| `coexistence` | I | every rank has **both** gpu_access and ib_queue_pair rows |
| `gpu_traffic` | D | same as 04: 196608 per op per rank |
| `owner_in_range` | I | `mem_dev` < `n_pes` |
| `ib_entities`, `regions_per_rank` | G | keep — NVSHMEM bootstrap, not derivable |

NB `regions_per_rank` and `ib_entities` are **failing right now** (115→114,
460→456) and fail at `5a28119` too. Diagnosing that is out of scope here; this
plan makes the case's *derivable* half assertable so it is not all-or-nothing.

### 07_nvshmem_multinode — `putget 65536 3`, 8 PEs, 2 nodes

The flagship: one execution, NVLink and IB on the same rank space.

| assert | kind | statement |
|---|---|---|
| `cross_transport_shape` | D | exactly `nodes` ranks (the node-crossing senders) have **0** NVLink accesses; the other `ranks - nodes` have `elems*iters` = 196608 |
| `ib_carries_the_crossing` | I | the zero-NVLink ranks are exactly the ranks with the highest IB volume |
| `owner_in_range` | I | `mem_dev` < `n_pes` |
| `ib_volume_by_rank` | G | keep |

Stated this way the case asserts the *physics* — inter-node NVSHMEM moves
payload by descriptor and NIC, so an address tracer cannot see it — without
naming which PEs happen to be the crossers. Today's golden names them (3 and 7)
and would break on a different `--map-by`.

### 08_ib_send_bw — `ib_send_bw -n 1000 -s 8192`, 2 ranks, 2 nodes

The WR-API path; mostly deducible.

| assert | kind | statement |
|---|---|---|
| `traffic` | D | the client posts exactly `iters` sends of `size` = 8192000 B total |
| `sends_have_payload` | I | no send has `length = 0` — the regression that motivated this case (202,000 poll_cq, **zero** post_send) |
| `regions` | D | 1 region per rank |
| region byte count (16384) | G | keep — perftest's own buffering, `2*size` is a guess not a derivation |

## Cross-cutting assertions

Worth applying to **every** case that has the table, in one shared file rather
than per case:

- `mem_dev < gpus_per_node` (or `n_pes`) wherever `gpu_access` exists
- no completion with a non-`success` status wherever `ib_poll_cq` exists
- no `ib_post_send` with `length = 0`
- **every expected table is non-empty** — the repo has already been burned by two
  identical `no such table` errors reading as "STABLE"
- record counts are consistent with the stream header's `records` column

That last pair is the cheapest and catches the worst failure: a capture that is
empty for an environmental reason, silently passing checks that find nothing to
contradict.

## Steps

1. `sql_check.sh`: read `meta.json`'s `workload`, create the `params` table,
   run `asserts/*.sql`, treat non-empty output as failure with the rows printed.
   Report golden and assertion results separately in `result.json`.
2. Add `workload` to all eight `meta.json` files (values above; they come
   straight from each `job.sbatch`).
3. Write the cross-cutting assertions once, included by every case.
4. Convert per case, **in this order** — highest value first:
   - **01** (all four derivable; fixes tonight's `qp_num` red)
   - **03** (the reference for correct attribution)
   - **08** (guards the WR-API regression)
   - **04** (`peer_fanout` = 1 lands `expect_fail`, becomes the acceptance test
     for the attribution plan)
   - **07**, **06**, **02**, **05** (shape and invariants; goldens stay)
5. Make it a rule, in the README's "Adding a case" checklist: a new case needs
   at least one derived assertion. A golden alone is not a test of correctness.
6. Only then consider retiring goldens that the assertions fully subsume — 01
   and 03 probably qualify. Not before; they are the regression net while the
   assertions are new.

## Acceptance

- ✔ `01` passes on an allocation that reuses a `qp_num` (tonight's failure
  mode). All four assertions green on 44111484 and 44113361, both of which
  collide; the topology golden is red on both.
- ✔ `03` unchanged — it is the control. Six assertions green on both runs,
  including `peer_topology` at `(run_dev+1) % gpus` and `owner_in_range`.
- ✔ `04`'s `peer_fanout` lands `expect_fail` and fails on both stored runs, at
  `peers = 2` for every device.
- ✔ `owner_in_range` fails on the `mem_dev = 64` capture (07, job 44111484) and
  passes on 44113361 and 44116827 — no cluster time used.
- ✔ Every case reports both verdicts, in the log and in `result.json`'s
  `goldens` / `asserts` objects.

## What the data changed

Validated by running the new `sql_check.sh` against every stored CSV export in
`~/pcomm-suite` (jobs 44111484, 44113361, 44116827). Four things this plan
asserted turned out not to hold, and one new bug fell out.

1. **01's `topology` cannot claim "and it is not itself".** Both stored pingpong
   captures collide on `qp_num` (449 in 44111484, 461 in 44113361) and both
   report `0 -> 0`. The assertion as landed is fan-out 1 carrying `iters` sends
   of `msg_bytes`, which passes on both; the self-edge is a `comm_edges`
   limitation, not a pingpong property, and asserting it would just move the
   false red from the golden to the assertion.
2. **02's `degree` — "every rank's peer count is equal, > 0" — is false today.**
   Job 44113361's edges name only `dst_rank` 0 and 3, with degrees alternating
   1 and 2 across the eight ranks. Per-rank send *totals* are a flat 3360, so
   the assertion landed as `symmetric_participation` (all ranks post, and post
   equally) and peer count is not asserted at all. The existing degree golden
   fails on both stored runs, independently of this work.
3. **04's `traffic` as written here re-encodes the bug.** "per rank per op:
   `elems*iters`" accepts 196608 loads *and* 196608 stores. `gpu_access` is
   peer-only by construction (`engine.hpp`'s `RemoteFilter` keeps an access iff
   `run_device != mem_device`) and `put_to_next`'s only peer access is the
   store — `src[i]` is a local read. `07_nvshmem_multinode`, running the
   identical binary with correct attribution, records **zero** loads. So the
   loads are phantom, the doubled count is this bug, and the derivation splits:
   `traffic` (stores only) passes, and a new `no_local_loads` fails
   `expect_fail` alongside `peer_fanout`.
4. **`owner_in_range` fails on more than case 07.** It fails *constantly* on 04
   and 06 (`mem_dev` 65..69 and 238..242 on a four-GPU node), not only on the
   corrupted multinode run — so the heap attribution fault is the single-node
   steady state, not just a multinode timing race. It also fails on **05**
   (`mem_dev` up to 8), which NVSHMEM cannot explain and
   `PLAN-20260803-1448` does not cover; that is a separate, undiagnosed
   instance now recorded in `things-to-circle-back-to.md`.
5. **New bug, found by the cheapest check in the plan.** `expect_tables` caught
   `08_ib_send_bw`'s `ib_queue_pair` being **empty on every capture we have**,
   with three green goldens beside it: perftest creates its QP through
   `mlx5dv_create_qp` and only `ibv_create_qp` is hooked, so the QP's modify
   records refer to an entity nothing ever created. Exactly the third door in
   the class case 08 exists to cover.

Deviation from the mechanism as written: the `size` param is spelled
`msg_bytes`, because `size` would have meant bytes for the IB cases and elements
for the GPU ones. `elems` / `elem_bytes` carry the latter.

Step 6 (retiring goldens that the assertions subsume) is **not** done and should
stay undone for now: 01 and 03 probably qualify, and the goldens are the
regression net while the assertions are new.

## Notes

- Assertions run against the **same CSV export** as the goldens, so this adds no
  capture cost and no new dependency — `sql_check.sh` already has sqlite.
- The one real risk is writing an assertion that merely re-encodes today's
  output in SQL. The rule that keeps it honest: **every assertion must reference
  `params`, or be an invariant that mentions no specific rank, device, or QP.**
  If it names an identifier the run produced, it is a golden wearing a costume.
