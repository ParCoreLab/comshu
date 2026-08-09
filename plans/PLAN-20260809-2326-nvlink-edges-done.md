# PLAN — the communication matrix does not see NVLink

- **Created:** 2026-08-09 23:26 +03
- **Status:** IN PROGRESS — steps 2, 3 and most of 4 shipped 2026-08-09 against
  the existing corpus, using a LID-derived node identity. Step 1 (the node
  record) is the remaining work and is what makes pure-NVLink captures resolve.

## Shipped 2026-08-09

**Steps were reordered, deliberately.** Step 1 is a capture-side schema change
needing a queue slot to validate; steps 2-4 are reader-side and turned out to be
verifiable *today* by deriving node identity from LIDs. Doing the reader half
first meant the corpus could prove the design before any capture was re-taken.

`nvshmem_coexist` went from **0 edges to a 4-rank NVLink ring**. And the full
case-07 ring is now visible in one matrix for the first time:

```
0→1→2→3   nvlink        3→4  verbs (196,620 sends)
4→5→6→7   nvlink        7→0  verbs
```

Ranks 3 and 7 originate **no** NVLink edges, which is the asymmetry
`tests/hpc/suite/README.md` documents in prose — now a checkable property of the
matrix rather than a paragraph.

**The oracle held on all nine captures**: the `transport IN ('verbs','devx')`
subset is still byte-identical to `CsvAgg`'s `comm_edges`.

Four things the implementation found that the plan did not anticipate:

- **`gpu_access` alone cannot build the device map.** Ranks 3 and 7 have *zero*
  `gpu_access` rows — they are the node-crossers — so `run_device` never names
  their GPUs, and rank 2's stores to device 3 would resolve to nothing.
  `mem_object.device` covers all eight ranks and is now the primary source, with
  `run_device` unioned in as a cross-check.
- **`min(src_lid)` per rank is not a safe node key.** Ranks on one node need not
  use the same set of ports. `_node` takes one level of closure — the lowest LID
  among every rank sharing a port with me — which survives partial overlap.
- **`link_health` reported a clean bill over 100% unattributed traffic.** It
  counted only IB sends, so `nccl_p2p` — no IB, therefore no LID, therefore no
  node — showed `unresolved_sends = 0` while every one of its 1.18M NVLink
  accesses resolved to `dst_rank = -1`. There is now an `unresolved_nvlink` row.
  This is the failure mode the health table exists to prevent, found in the
  health table itself.
- **Captures with no IB resolve to honest unknowns, not guesses.** `nccl_p2p`
  and `p2p_kernel` produce edges with `dst_rank = -1`. That is the correct
  answer until step 1 lands, and the UI already renders that column.

## Step 1 shipped 2026-08-10 — `NodeIdentity`, type 524

`PCOMM_RECORD(NodeIdentity, RecordKind::Entity, 524, "node_identity")`, one
`uint64` FNV-1a of the hostname, emitted once per rank by the engine at start
beside the clock anchor. Schema hash `0x160D1B71CE3BED36` →
`0x6EC51373F67692CA`; `flatc` regenerated (C++ and Python), conformance case
added, `schema_check` green at 28 records.

A hash rather than the hostname: the id only has to be comparable between ranks
of one job. `node_hash = 0` records that `gethostname` failed and is treated by
the reader as *absent*, not as a node — otherwise every failed rank across a job
would merge onto one fictional machine.

**The record wins, the LID inference stays as fallback.** `_node` prefers
`node_identity` and falls back to `_node_by_lid`, so captures written before this
record still resolve exactly as they did — verified, zero regressions across all
nine corpus captures, oracle still byte-identical.

**Verified end to end.** A locally produced capture contains `524.pcs`, and its
value matches FNV-1a of the hostname computed independently
(`simurgh-base → 6636794384584331992`). Replaying `nccl_p2p` — the pure-NVLink
capture with no IB and therefore no LID — with a node identity present turns its
matrix from **four fully unresolved edges into a 4-rank ring**:

```
before   0→-1, 1→-1, 2→-1, 3→-1        (294,987 accesses each, all unknown)
after    0→1, 1→2, 2→3, 3→0            (294,948 each, plus 39-access reverses)
         link_health: clean
```

**Two-node case verified on MN5** — job 44451168, case 07 re-run on the new
schema, PASS (15 checks, 1 xfail). `524.pcs` is present in every rank, and the
acceptance criterion is met exactly:

```
ranks 0-3  node_hash 2581583022447974712
ranks 4-7  node_hash 2581590719029372189      2 nodes, 8 ranks
```

And the two independent methods agree: the NVLink edges derived from the
*recorded* identity on this capture are identical to those derived from the
*LID inference* on the pre-change capture of the same workload. That is the
fallback validating the record and the record validating the fallback, which is
worth more than either alone.

The full hybrid ring, from real node identity:

```
nvlink  0→1  1→2  2→3      4→5  5→6  6→7
verbs        3→4                      7→0
```

## Step 4 completed 2026-08-10 — unit awareness

**The Ranks panel had silently started summing across transports** when NVLink
landed — rank 0 read "790,372 bytes sent", which was 196,608 NVLink 4-byte
stores plus ~91 InfiniBand messages added together. A number with no meaning,
presented as a total, and introduced by this very plan. It is now small
multiples: one section per transport, each with its own bar scale, labelled
"measured in accesses — not comparable with the other sections". Per-transport
scales are not cosmetic — a 4 MB NVLink ring beside a GB of IB would otherwise
be an invisible sliver.

**The Flow panel excluded NVLink entirely** (it reads the raw send tables, not
comm_edges). It now includes it, with the ops NAMED rather than numbered: both
planes number from 0 and mean different things — a verbs opcode 1 is
write-with-immediate, a MemOp 1 is a Store — so rendering both as "opcode 1"
would merge two unrelated things into one node.

This plan is complete. Remaining work belongs elsewhere: naming verbs/DEVX
opcodes needs the derived table `PLAN-20260807-1718` step 2 still owes, and
`gpu_access.object` attribution (0 of 1.18M rows attributed) is its own gap.
- **Depends on:** nothing built. Step 1 below is a capture-side change (a new
  record type); steps 2-4 are reader-side and land on the existing corpus.
- **Related:** `PLAN-20260807-1718-ui-backbone-todo.md` (this is the correctness
  hole under its headline panel, and its step 2 owes the derived tables this
  extends); `PLAN-20260806-1329-generic-memory-attribution-todo.md` (the same
  captures show 75-96% of `mem_object.device` unresolved — a different gap in
  the same data, not fixed here).

## What is broken

`comm_edges` is built entirely from `ib_post_send` and `mlx5_device_send`. GPU
memory traffic over NVLink is not in it, so on a single-node run the
communication matrix is **empty over a capture full of communication**.

Measured 2026-08-09 on the corpus:

```
nvshmem_coexist-44400747   comm_edges rows: 0
                           gpu_access where run_device <> mem_device: 786,432
```

The data is present and clean. `nvshmem_multinode-44400747` is a ring in
`gpu_access` alone:

```
rank  run_device -> mem_device   accesses    bytes
0     0 -> 1                      196,608     786,432
1     1 -> 2                      196,608     786,432
2     2 -> 3                      196,608     786,432
```

This is not a missing feature, it is the headline view contradicting the
project's own claim. `tests/hpc/suite/README.md` says of case 07 that inter-node
hops move no payload through GPU loads/stores while intra-node hops move all of
it, and that **"pairing the two providers is what makes the whole ring
observable"**. The matrix currently observes one half and presents it as the
whole.

## The three problems, in the order they bite

### 1. A peer device does not name a rank

An NVLink edge is `(rank, run_device) -> mem_device`. Turning `mem_device` into
a rank needs `(node, device) -> rank`, and **nothing in a capture records which
node a rank ran on**. Rank 0 and rank 4 both report `run_device = 0` on
`nvshmem_multinode`; they are on different nodes and are different GPUs.

Options considered:

- **Infer the node from `src_lid`.** Ranks sharing a LID share an HCA, hence a
  node. Free where IB exists — and useless exactly where it is most needed:
  `nccl_p2p` and `p2p_kernel` have no `ib_*` streams at all, so a pure-NVLink
  run has no LID to group by. Rejected as the primary mechanism, kept as a
  cross-check.
- **Assume single-node when no IB is present.** True for every current case and
  false the moment it is not, silently. Rejected: this is the class of
  assumption that produced the `qp_num` self-edges.
- **Record node identity in the capture.** A new Entity in the internal range
  carrying a stable node id per rank. Chosen: it is the only option that is
  correct without IB, and it is small.

### 2. NVLink bytes and IB bytes are not the same quantity

Measured: every peer access in `nvshmem_multinode` is `op = Store`, `size = 4`.
1,179,648 stores × 4 B = **4.7 MB**, against 1.3 GB of IB payload per edge on
`scale_nccl`. Summing those into one `bytes` column produces a number that means
nothing, and a matrix coloured by it would rank a 4.7 MB NVLink ring below a
single IB message.

They differ in kind, not just scale:

- an IB row is **one message the application asked to send**;
- a `gpu_access` row is **one instrumented load/store instruction**, and only
  from kernels NVBit was told to instrument. Case 04 sets `PCOMM_KERNEL_NAME` to
  keep the capture bounded, and case 05's own meta.json records that LL-protocol
  polling makes the access count *not repeatable* run to run.

So NVLink volume is a lower bound produced by a sampling-shaped filter, and IB
volume is exact. **They must never be added.** The design consequence is below.

### 3. Direction is carried by the op, not by the row

A Store to peer memory pushes data out; a Load pulls it in. The edge direction
follows the op, and only `Store` appears in the corpus today — which means the
Load case will be exercised for the first time by whatever capture has one, and
should be written correctly now rather than discovered later.

## Design decisions

**`transport` becomes a column of `comm_edges`, and rows are never merged
across it.** One matrix, faceted by transport, rather than two tables the UI has
to know to union. `sends` keeps its meaning per transport (messages for IB,
instrumented accesses for NVLink) and is joined by an explicit `unit` column so
no consumer has to infer it.

**The oracle relationship is preserved by subsetting, not abandoned.** The SQL
`comm_edges` currently matches `CsvAgg`'s CSV output byte-for-byte on all nine
captures, and that equality is the only check the derived layer has. After this
change the invariant becomes: *the subset where `transport IN ('verbs','devx')`
equals `CsvAgg`'s `comm_edges`*. Keep it as an executable check, not a comment.

**`CsvAgg` is not extended.** The suite's goldens and assertions read its CSV,
and NVLink rows appearing there would move every topology golden for reasons
that have nothing to do with what those goldens test. The SQL layer is where the
graph grows.

## Steps

### Step 1 — record node identity (capture side)

A new Entity in the internal range (`524` is free; 520/522 are taken) carrying a
stable per-rank node identifier — the hostname hash is sufficient and avoids
putting a hostname string on disk for every rank.

Emitted once per rank at startup by the core, not by a provider: every provider
needs it and none owns it. This is the same reasoning that moved `MemObject`
into the internal range.

Acceptance: a two-node capture yields exactly two distinct node ids, ranks 0-3
sharing one and 4-7 the other, and cross-checks against `src_lid` grouping on a
capture that has both.

### Step 2 — `(node, device) -> rank` in the link pass

A `rank` table — the one `PLAN-20260807-1718`'s step 2 already owes — with
`rank, node, devices, lids`. `devices` comes from `gpu_access.run_device`
observed per rank; the map is then an inversion of it.

**Scope every lookup by node.** This is the `qp_num` lesson in the device
domain: device ordinals are node-local and collide across nodes exactly as
`qp_num` does across HCAs, and a `mem_device`-keyed map without the node would
attribute one node's GPU to another node's rank as a matter of course.

Acceptance: on `nvshmem_multinode`, every `(node, device)` resolves to exactly
one rank, and no rank claims a device on two nodes.

### Step 3 — NVLink edges into `comm_edges`

Add `transport` and `unit` columns; add a `_nvlink_sends` view beside
`_sends`:

```sql
SELECT rank AS src_rank, <peer rank of (node, mem_device)> AS dst_rank,
       count(*) AS sends, sum(size)::BIGINT AS bytes, 'nvlink' AS transport
FROM gpu_access WHERE run_device <> mem_device AND op = <Store>
GROUP BY 1, 2
```

with the Load case reversing src and dst rather than being filtered out.

Unattributable peers become `dst_rank = -1`, the same way an unresolved IB peer
does — the UI already renders that column and it is the signal a node map has a
hole.

Acceptance, all executable:
- `nvshmem_coexist` stops being an empty matrix and yields a 4-rank NVLink ring;
- `nvshmem_multinode` yields NVLink edges for ranks 0-2 and 4-6 and **none** for
  ranks 3 and 7, which cross nodes and legitimately move nothing over NVLink —
  the asymmetry the suite README already documents, now visible in the matrix;
- the `transport IN ('verbs','devx')` subset is still byte-identical to
  `CsvAgg`'s `comm_edges` on all nine captures.

### Step 4 — the UI must not present incommensurable units as one number

The matrix panel gets a transport facet. Default to showing them separately;
if a combined view is ever added, it must colour by *rank order within
transport* rather than by raw bytes.

`sends`/`bytes` labels become unit-aware: "messages" for IB, "4 B stores" (or
whatever the capture's access granularity is) for NVLink. The legend must say
which, because a legend reading "bytes sent, log scale" over mixed transports
would be a lie of exactly the kind the unresolved-peer column exists to prevent.

Acceptance: a capture with both transports renders both without either being
invisible, and no view sums across transports.

## Risks

- **The temptation to sum.** Every step above exists to stop one number being
  produced from two measurements. If a reviewer asks for "total bytes moved",
  the answer is two numbers and a sentence, not one number.
- **NVLink volume is a lower bound.** Kernel filtering and non-repeatable
  polling counts mean this axis is not comparable run-to-run the way IB is. Say
  so in the UI, not only here.
- **`gpu_access` is the firehose** — 1.2M rows per capture at 8 ranks, and the
  aggregate is per `(rank, mem_device)`, so it is a full scan of the largest
  table in every link pass. Measure it before assuming it is free; the corpus
  index has the row counts.
- **Step 1 is a schema change** and every golden carrying the schema hash moves.
  Do it as its own commit, as `PLAN-20260806-1329` step 2 prescribes.

## Explicitly out of scope

- **`gpu_access.object` attribution.** Every one of the 1,179,648 accesses in
  the corpus has `object = 0`, so object-level views remain impossible. That is
  a real gap and a different one.
- **`source_line`** — empty in all nine captures; a capture-side gap, not a
  reader one.
- Merging NVLink into `CsvAgg`, for the golden reasons above.
- Inferring node identity from timing, allocation addresses, or rank ordering.
  All three are guesses that would work on the corpus and fail in the field.
