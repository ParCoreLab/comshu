# PLAN — host-initiated CUDA memory copies are invisible

Status: done
Created: 2026-09-03 18:28
Landed: 2026-09-03, validated on du04 (4x A100-SXM4-40GB, CUDA 13.0, driver 580.95.05)

## The gap, stated exactly

`pcomm/run.cpp`'s registry says it out loud, in the `gpumem` row:

> It does not capture DMA: NVBit rewrites SASS, so a copy-engine cudaMemcpy
> executes no instruction and is invisible here.

That sentence is true of every provider in the tree, not only of `gpumem`.
A copy engine moves bytes without retiring an instruction on any SM, without
posting a work request on any queue pair, and without calling into UCX or NCCL.
So a `cudaMemcpy` appears in **no** table today.

The part that makes this a hole in a *communication* profiler rather than a
bookkeeping omission: **`cudaMemcpyPeer` and device-to-device copies are
communication.** They move bytes between two GPUs over NVLink or PCIe, they are
what a hand-rolled halo exchange or a model-parallel weight shuffle is built
out of, and they resolve to a rank→rank edge exactly as an `ib_post_send` does.
`comm_edges` has lanes for `verbs`, `devx`, `nvlink` and `ucx` and none for
this, so a workload whose entire inter-GPU traffic is `cudaMemcpyPeerAsync`
produces an **empty communication matrix** and a clean `link_health`.

## Decision

Add one record — **`CudaMemcpy`, Event, type_id 534, in the ENGINE's internal
range** — measured by **CUPTI's activity API** (`CUPTI_ACTIVITY_KIND_MEMCPY`
and `CUPTI_ACTIVITY_KIND_MEMCPY2`) in the existing `gpu-backends/cupti`
backend, and give the derived layer a `cudamemcpy` transport lane in
`comm_edges` / `edge_sizes`, a `cuda_memcpy` lane in `timeline_span`, and two
new `link_health` rows.

Everything below is the reasoning for each half of that, including the parts
that were rejected.

---

## A. Which door — CUPTI activity, host-API interposition, or both?

**Decision: CUPTI activity now. Host-API interposition named, scoped out, and
provided for by the record's `source` field.**

The tree's repeated pattern is real and it applies: `ib` vs `ibgda`, `ucx` vs
`ucx_deep`, NCCL-asked (900-910) vs NCCL-chose (912-916). A `cudaMemcpyAsync`
call and the copy the engine performed are two planes. They are not near each
other in time either — `GpuKernel`'s comment measures launch-to-execution at
9-13 ms against 11-20 us kernels, and an async copy decouples exactly the same
way.

So why only one plane in this change:

1. **The execution plane is the one that carries the communication fact, and
   the API plane cannot be trusted to exist.** `cudaMemcpy*` is in libcudart,
   and **nvcc links libcudart statically by default**. `cupti_backend.cpp`
   already carries the measurement: a statically linked cudart reaches the
   driver through `cuGetProcAddress`, "which defeats symbol interposition
   entirely (measured during the IBGDA work)". So the interposition door is
   *absent on an ordinary `nvcc -o app app.cu`*, which is most of the corpus.
   CUPTI is inside the driver and cannot be routed around this way.

2. **Only CUPTI names both device ordinals.** `CUpti_ActivityMemcpyPtoP4`
   carries `srcDeviceId` and `dstDeviceId`. That pair *is* the edge. The API
   door would have to recover it from `cuPointerGetAttribute` on both pointers
   — a second driver call per copy, on the hot path, for a fact CUPTI reports
   for free.

3. **The API door is not free of the same conflict.** It would arm from
   `LD_PRELOAD` and would therefore be exposed to whatever else is in the
   chain, and it would still have to answer to `PCOMM_KERNEL_TIMING`-shaped
   questions. It is a second instrument, not a fallback that is obviously
   available.

What the API door would buy, and this is the honest cost of deferring it:

- **Addresses.** Neither `CUpti_ActivityMemcpy6` nor
  `CUpti_ActivityMemcpyPtoP4` carries a source or destination pointer — read
  the two struct definitions in `cupti_activity.h`; the field lists are
  `copyKind, srcKind, dstKind, flags, bytes, start, end, deviceId, contextId,
  streamId, correlationId, ...` and for PtoP additionally `srcDeviceId,
  srcContextId, dstDeviceId, dstContextId`. **There is no address anywhere.**
  This is why `object_access` is out of scope below, and it is the single
  strongest argument for eventually adding the API plane, which has the
  pointers in its arguments.
- **Coverage under `gpumem`.** NVBit owns the CUPTI subscriber, so a `-p
  gpumem` capture can have no copy rows at all. An interposed `cuMemcpyPeer*`
  needs no subscriber and would compose with NVBit. See E.

Both of those are recorded as follow-ups rather than done here, and the record
is shaped so that adding them is a new `source` value and no schema break.

**Rejected: the NVBit backend as a second instrument.** It cannot be one. The
premise of this whole plan is that a copy-engine transfer retires no
instruction, so there is nothing for NVBit to instrument. This is the one way a
memcpy record differs from `GpuKernel`, which genuinely has two instruments
today.

## B. Which record-id range?

**Decision: 534, in `internal_lo = 500 .. internal_hi = 599`, beside
`GpuKernel` (530). `id_range::cupti_lo = 300` stays reserved and unused.**

`GpuKernel`'s own comment makes the argument and it transfers, with one
adjustment that has to be stated rather than assumed:

> `id_range::cupti_lo = 300` was reserved for this and it is the wrong home: it
> names an INSTRUMENT, and this record has two of them.

`CudaMemcpy` has **one** instrument today, so that sentence is not literally
true of it. The argument still holds, for the reason underneath it:

> "does this capture know X" must not answer "only if you ran NVBit".

Put the record at 300 and the id map says *this fact is a property of having
run CUPTI*. It is not. It is a property of the process having copied memory,
and the plan above already names a second instrument for it (host-API
interposition) which needs no CUPTI at all and which is specifically the one
that would work under `gpumem`. Numbering it in the CUPTI range would make the
follow-up either emit a record from a range whose name is a lie, or mint a
second record family for the same fact — the exact split `KernelSource`
exists to avoid.

It also lands where the joins are. `GpuKernel.correlation` and
`CudaMemcpy.correlation` are the same CUPTI correlation-id space, and
`GpuAccess.launch` already joins into it. Three records in one join space, two
of them at 530 and 534, is a readable id map.

**Should the 300 range be reconsidered?** No, and it should be documented as
declined rather than merely left empty. Two records in a row have now looked at
it and gone elsewhere for the same reason. `core/ids.hpp` gets a sentence
saying so, so the third does not have to re-derive it. Deleting the range is
not proposed — the ranges are stable ids and shrinking the map is a change with
no benefit.

## C. Does it need a `source` field?

**Yes.** For `GpuKernel`'s reason, one step weaker but the same shape: what
`[a, b]` MEANS depends on the instrument.

- `CopySource::Cupti` — the copy engine's execution window. What this change
  produces.
- `CopySource::HostApi` — the API call. For a **synchronous** `cudaMemcpy`
  that is a genuine bound on the transfer (the call blocks until it completes);
  for `cudaMemcpyAsync` it is submission and is 9-13 ms away from the truth.

Writing the enumerator now, with no producer, is the same choice
`_nvlink_sends` made for its `Load` branch: "written now rather than discovered
by whichever capture first has one". `_tl_raw` filters `source <> 1` on the
copy lane from day one, exactly as it filters `source <> 2` on the kernel lane.

**Does `link_health` need a companion row?** `GpuKernel`'s is
`kernel_rows_submission_only`. **No — deliberately not, yet.** No producer can
emit `source = HostApi` today, so the row would be structurally zero on every
capture that will ever exist until the API door lands. `link_health`'s own
history is explicit that a row which cannot say anything is worse than no row
(`ucx_edges_include_intranode` was deleted for firing on every healthy capture;
the inverse failure — a row that can never fire — trains the same
inattention). The row lands in the same change as its producer.

Two rows that **can** fire are added instead; see D and E.

## D. What does the derived layer do with it?

Evaluated one at a time, including the two that are out of scope.

### comm_edges / edge_sizes — YES, a new `cudamemcpy` lane

`transport = 'cudamemcpy'`, `unit = 'bytes'`.

The edge rule, and it is deliberately **not** `kind = PeerToPeer`:

> A copy is an edge iff **both device ordinals are known and they differ**.

Keyed on the devices rather than on CUPTI's `copyKind` because `copyKind` is a
claim about the API that was called and the device pair is a fact about where
the bytes went. A `cudaMemcpy(..., cudaMemcpyDeviceToDevice)` between two
buffers on different devices under unified addressing is a genuinely open
question — CUPTI may report it as `MEMCPY`/`DTOD` with a single `deviceId`, in
which case `src_device == dst_device` and it is correctly not an edge, or as
`MEMCPY2`/`PTOP` with two. **This is measured on du04, not assumed** (see the
hardware plan); either way the device-pair rule gives the right answer and the
`copyKind` rule would not.

H2D and D2H are **not** edges. `comm_edges` is a rank→rank matrix and a host
staging copy is not rank→rank traffic. They stay in the raw table and on the
timeline, where they belong.

Peer resolution goes through the existing `_node` / `_device_key` /
`_device_owner` machinery unchanged, so a copy resolves the same way an NVLink
store does. `edge_sizes` gets the matching size-classed lane, because
`tests/comm_edges_rule.sh` asserts the roll-up is exact and a lane in one and
not the other fails it — which is the point of that assertion.

`_device_key` gains the copy's devices as key sources (so an ordinal that
appears only in a copy still resolves). `_device_owner` **does not**, and that
asymmetry is load-bearing: the far end of a peer copy is a device the rank does
*not* own, exactly as `gpu_access.mem_device` is, and `_device_owner`'s own
comment refuses buffer residency for that reason — "a missing owner is
recoverable, a confidently wrong one is not". Only the non-peer rows (where
CUPTI's single `deviceId` is by construction the context's own device) are
admitted as ownership evidence, in `_device_owner` and `_rank_device`.

### timeline_span — YES

A `cuda_memcpy` lane in `_tl_raw`, `bytes` populated, `kind = 2` (Interval,
from the record's own temporal). No clock problem: the CUPTI backend already
substitutes our TSC for CUPTI's timebase and records the *verified* domain in
`g_domain`, so copies land in domain 0 alongside every host record and ride the
anchors the engine already emits. If the substitution is declined at runtime
they land in `kDomainCuptiWall` (1) and are anchored by `emit_anchor()`, which
already runs. Either way `link_health.timeline_unanchored` is the existing net.

### object_access — NO, and this is a hard structural limit, not a shortcut

**CUPTI's memcpy activity records carry no addresses.** Named precisely, so the
negative result is checkable: `CUpti_ActivityMemcpy6` and
`CUpti_ActivityMemcpyPtoP4` in `cupti_activity.h` (CUDA 13.0) have no `src`,
`dst`, or any pointer-valued field. There is nothing to join to `mem_object`
with. This is not "deferred because it is work"; from this door it is
impossible, and it is the concrete thing the host-API plane would unlock.

### link_health — YES, two rows, both able to fire

1. `unresolved_device_copy` (**defect**) — peer copies whose src or dst rank
   could not be resolved. The direct analogue of `unresolved_nvlink`, and it
   fails for the same reason (no node identity, or no owner for a device key).
   Without it a capture whose entire copy matrix collapsed to `dst_rank = -1`
   reports a clean bill of health, which is the failure `unresolved_nvlink` was
   added for, arriving on a fourth key.

2. `memcpy_capture_unavailable` (**note**) — this run loaded `gpumem`, so
   CUPTI was refused and the copy lane is **structurally absent rather than
   empty**. Read from `run_provenance.env`, which carries `PCOMM_CUPTI_ALLOWED`
   because the launcher sets it and `Invocation`'s allowlist keeps every
   `PCOMM_*` variable. This is the answer to E and it is the row this feature
   most needs: "an absent provider and a broken one look identical downstream"
   is the recurring lesson (the IBGDA plan, case 08), and here the *third*
   possibility — "the copy engine was idle" — is also identical. Severity
   `note`, not `defect`: running `-p gpumem` is a legitimate choice, not a
   fault.

## E. The CUPTI/NVBit conflict

Nothing here makes it worse, by construction: the copy activity kinds are
enabled inside `enable_once()`, **after** both existing guards
(`pcomm_engine_kernel_timing_wanted()` and `pcomm_engine_cupti_allowed()`) and
after `cuptiActivityRegisterCallbacks` has already succeeded. A process where
NVBit owns the subscriber never reaches the new code, and `build_passes()` in
`pcomm/run.cpp` is untouched.

What is added is the *statement*, per D.2: a capture taken with `gpumem`
loaded now says in `link_health` that its copy lane was disabled, instead of
presenting an empty table that reads as "this workload copied nothing".

`PCOMM_COPY_CAPTURE=0` turns the two activity kinds off while leaving kernel
timing on. Read directly in the backend rather than added to `engine_abi.h`,
because only one backend can answer it — `pcomm_engine_kernel_timing_wanted()`
is in the ABI precisely because *two* backends consult it, and that is the
distinction that comment draws.

---

## Constraints that must be honoured

- **Four-layer checklist** (README): `PCOMM_RECORD` → `pcomm.fbs` struct +
  `kSchemaHash` → `schema_conformance.cpp` case → `Table<>` + `TableRecords`.
  Plus two the README does not list and the build enforces anyway:
  `tools/gen_raw_columns.py`'s `ENUMS` set is **closed by construction** and
  hard-errors on an unknown enum, and `link_sql.hpp`'s `vocabulary` table must
  publish every enum a record field uses (`schema_check.py` §5b).
- **Vendor-free rule.** Every CUPTI type stays in `gpu-backends/cupti/`. The
  engine gains a C ABI entry point taking scalars only.
- **Enum columns export as NUMBERS, not names.** `sql_check.sh` runs assertions
  against the **Parquet** export when one exists and falls back to CSV+sqlite
  when it does not — and the Parquet raw tables are generated from the record
  structs, where an enum is a `UTINYINT`. `gpu_access.op` prints `Store` in the
  CSV and `1` in the Parquet, so no single assertion can name it. A new record
  should not add a second instance of that. Numbers in both; the `vocabulary`
  table is where a name comes from.
- **No ABI version bump.** New entry point only, on the precedent and the
  reasoning `pcomm_engine_note_qp` documents: the version is compared for
  equality at declare, so bumping would reject providers this addition does not
  break, while a provider built against the new header gets
  `undefined symbol: pcomm_engine_record_memcpy` from an old engine rather than
  silent mis-accounting.
- **Never assert what the tool emitted.** Every suite assertion derives from
  `meta.json`'s `workload` block, which is derived from the workload source.
- **Mutation-test, then empty-input-test, every assertion.**

## Steps

1. `core/vocab.hpp` — `MemcpyKind`, `MemResidency`, `CopySource`.
2. `core/records.hpp` — `PCOMM_RECORD(CudaMemcpy, Event, 534, "cuda_memcpy")`.
   `core/ids.hpp` — the sentence about why 300 is still empty.
3. `schema/pcomm.fbs` — three enums + the struct; `kSchemaHash` from
   `schema_check.py`; regenerate `generated/` and `generated-python/`.
4. `libpcomm/tests/schema_conformance.cpp` — enum value asserts + a byte case
   with every field distinct and non-zero.
5. `engine_abi.h` + `libpcomm/src/engine.cpp` —
   `pcomm_engine_record_memcpy(...)`.
6. `gpu-backends/cupti/cupti_backend.cpp` — enable the two activity kinds,
   translate both record layouts, `PCOMM_COPY_CAPTURE`, counters.
7. `pcomm-read/main.cpp` — `Table<CudaMemcpy>` + `TableRecords`.
   `tools/gen_raw_columns.py` — the three enums.
8. `pcomm-read/link_sql.hpp` — `_memcpy_edges`, `_memcpy_edges_sized`,
   `comm_edges`/`edge_sizes` lanes, `_tl_raw` lane, `_device_key` /
   `_device_owner` / `_rank_device` evidence, two `link_health` rows,
   `vocabulary` rows.
9. `tests/comm_edges_rule.sh` — a `cuda_memcpy.csv` fixture and its
   assertions. This is the mutation-testable, hardware-free half.
10. `tests/hpc/cases/23_cuda_memcpy/` — `copybench.cu`, `meta.json` with a real
    oracle, `asserts/*.sql`, `run.sh`/`job.sbatch`.
11. Hardware: du04. Predict, then run.

## Hardware plan (du04, 4x A100-SXM4-40GB, NVLink, no scheduler)

`copybench <iters> <bytes> <ndev>` does, per iteration: one H2D, one D2H, one
same-device D2D, one `cudaMemcpy(...DeviceToDevice)` across two devices, and
one `cudaMemcpyPeer` for every ordered pair `i != j`.

Predictions to be written down *before* the first run:

| question | prediction |
|---|---|
| H2D bytes | `iters * bytes`, exactly |
| D2H bytes | `iters * bytes`, exactly |
| peer copies | `iters * ndev * (ndev-1)` rows, `kind = PeerToPeer` |
| peer bytes | `iters * ndev * (ndev-1) * bytes` |
| `src_device`/`dst_device` on a peer row | the two ordinals passed to `cudaMemcpyPeer`, in that order |
| cross-device `cudaMemcpyDeviceToDevice` | **unknown** — either `MEMCPY2`/`PTOP` with two ordinals, or `MEMCPY`/`DTOD` with one. This is the measurement the device-pair edge rule is designed to be right under either answer. |
| `comm_edges` | one `cudamemcpy` row, `0 -> 0`, `intra-node` (single process owns all four GPUs) |
| copy duration | non-zero and ordered; a 64 MiB peer copy over NVLink should be O(100 us), which is the sanity check that `[a, b]` is the engine's window and not the API call's |

MN5 has nothing multi-node to say about peer copies — they are intra-node by
construction — so an MN5 job is only worth a slot if the A100 result raises an
H100-vs-A100 question. It is not booked speculatively.

A second case with four MPI ranks and CUDA IPC (each rank owns one GPU, opens
its neighbour's handle, and `cudaMemcpyPeer`s into it) is what turns `0 -> 0`
into a genuine `r -> r+1` ring and is the real test of the rank-space lane. It
is planned as case 24 and is explicitly the lower-priority half: the record and
the lane are validated by case 23, and case 24 validates the *resolution*.

---

## What happened (du04, 2026-09-03)

`copybench 200 4194304 4` under `pcomm run -p cupti`. The predictions above were
written before the first run; here is each one against the measurement.

**Volumes — every prediction held exactly.** 4400 records in `534.pcs`,
stride 88 B, against the 4400 the workload counts for itself:

| class | copies | bytes | predicted |
|---|---|---|---|
| `kind=1` HostToDevice | 800 | 3,355,443,200 | 800 / 3,355,443,200 ✔ |
| `kind=2` DeviceToHost | 800 | 3,355,443,200 | 800 / 3,355,443,200 ✔ |
| `kind=3` DeviceToDevice, one device | 200 | 838,860,800 | 200 / 838,860,800 ✔ |
| `kind=5` PeerToPeer | 2600 | 10,485,760,000 | **2400 predicted** — see below |

`src_device` on every H2D row is 65535 and `dst_device` runs 0..3; D2H is the
mirror. `src_space`/`dst_space` come back Pinned→Device and Device→Pinned, which
is what `cudaHostAlloc` should produce and is a fact no other record in a
capture can state.

**The open question is answered: CUPTI reports a cross-device
`cudaMemcpyDeviceToDevice` as `CUPTI_ACTIVITY_KIND_MEMCPY2` with BOTH ordinals.**
The peer count is 2600, not 2400 — 2400 from `cudaMemcpyPeer` plus the 200
phase-5 probes, which arrive as `copyKind = PTOP` with `srcDeviceId = 0,
dstDeviceId = 1`. The device-pair edge rule is right under this answer and would
have been right under the other; a `kind`-keyed rule would have been wrong under
it. `edge_sizes` separates the two populations cleanly by size class, which is
what the deliberately different `uva_bytes` was for:

    size_class  sends  bytes
    2097152       200    419430400   <- the phase-5 probes
    4194304      2400  10066329600   <- cudaMemcpyPeer

**The peer matrix is exactly the workload's**: twelve ordered pairs, 200 copies
each, except (0,1) which carries 400 — 200 peer plus the 200 probes, which is
the arithmetic the two size classes above confirm.

**comm_edges** — one row, and it is the honest one for a single process that
owns four GPUs:

    src_rank dst_rank transport   unit   scope       sends  bytes
    0        0        cudamemcpy  bytes  intra-node  2600   10485760000

**Timing.** All 4400 rows are `temporal_kind = 2` (Interval) in domain 0 — the
TSC substitution took, so copies ride the anchors the engine already emits and
needed no new clock work. Durations from `timeline_span`, grouped by
destination device:

    ep      n     min_ns   avg_ns   max_ns
    0     1000      6,048   67,635  440,989
    1     1000     24,640   71,446  388,767
    2      800     44,928   81,735  316,063
    3      800     44,927   84,623  323,838
    65535  800    184,063  263,032  612,252   <- the host: D2H over PCIe

263 us for 4 MiB to the host is 15.9 GB/s, which is the right order for pinned
D2H on a contended PCIe Gen4 link; the device destinations average 67-85 us,
i.e. ~50-63 GB/s, the right order for NVLink with four GPUs all copying at once.
The point is not the bandwidth, it is that `[a, b]` is a *window* of the right
magnitude — a submission stamp would not be.

**link_health: no non-zero rows.** `unresolved_device_copy = 0`, so the
ownership chain closed; `memcpy_capture_unavailable = 0`, correctly, since this
run had no gpumem in it.

**`async` was validated separately**, because copybench is synchronous by
design and only ever exercises `async = 0`. A 12-copy ad-hoc probe (5 sync then
7 `cudaMemcpyAsync` on a created stream) came back
`kind=1 async=0 stream=7 n=5` and `kind=1 async=1 stream=16 n=7` — both the flag
and the stream ordinal carry.

## The multi-rank run, and the field it added

The single-process case cannot exercise the resolution chain: one process owns
all four GPUs, so every edge it can produce is `0 -> 0`. So a four-rank MPI
ring was run beside it — rank r opens rank (r+1)'s allocation with CUDA IPC and
`cudaMemcpyPeer`s into it, 100 copies of 1 MiB each.

**The first run of it produced a perfect device-space matrix and an unresolved
rank-space one:**

    rank src_device dst_device   n     comm_edges
    0        0          1       100    -1 -> -1  cudamemcpy  400  419430400
    1        1          2       100    link_health.unresolved_device_copy = 400
    2        2          3       100    rank_identity.devices = 0 on every rank
    3        3          0       100

Every record was right and the matrix was worthless, because **nothing in that
capture said which rank drove which GPU.** The evidence `_device_owner` accepts
is `mem_object` (cudaMalloc is deliberately not hooked, per engine.cpp's
comment), `gpu_access.run_device` (needs gpumem, which cannot share a process
with CUPTI), `gpu_kernel.device` (that workload launches no kernels) and
`nccl_comm` (no NCCL). A pure-peer-copy program supplies none of them, and
`gpu_device` was absent for the flush-path reason below.

**Neither endpoint of a peer copy can supply it.** A peer copy names one device
the rank owns and one it does not, and the row does not say which:
`cudaMemcpyPeer(dst, dstDev, src, srcDev, n)` is equally legal pulling from a
peer as pushing to one. Guessing would hand another rank's traffic to the wrong
owner, which is what `_device_owner`'s own comment refuses buffer residency for.

**CUPTI reports a third ordinal and it is the right one.**
`CUpti_ActivityMemcpyPtoP4` carries `deviceId` beside `srcDeviceId` and
`dstDeviceId`, paired with `contextId` — the context the copy was issued in,
which belongs to the process that made it current. The record gained
`ctx_device` for it (it fits in the existing tail padding, so the struct is
still 40 B and 88 B stamped), `_device_owner` and `_rank_device` take it as
strong evidence, and `_device_key` takes all three ordinals. Re-run:

    src_rank dst_rank transport   unit   scope       sends  bytes
    0        1        cudamemcpy  bytes  intra-node  100    104857600
    1        2        cudamemcpy  bytes  intra-node  100    104857600
    2        3        cudamemcpy  bytes  intra-node  100    104857600
    3        0        cudamemcpy  bytes  intra-node  100    104857600

`link_health` clean, `rank_identity` naming one device per rank, and
`ctx_device` equal to the issuing rank's own ordinal on all 400 rows — which is
the measurement confirming what `deviceId` means on the peer record. That run is
now `tests/hpc/cases/24_cuda_memcpy_ring`.

## What this cost, and what it found on the way

**Six enforcement layers, not four.** README's "Adding a record type" lists
four. This record hit three more that fail the build or a ctest:
`tools/gen_raw_columns.py`'s `ENUMS` set (closed by construction),
`link_sql.hpp`'s `vocabulary` table (`schema_check.py` §5b), and
`tools/gen_figures.py`'s `FAMILIES` plus a box in the family figure
(ctest `figures_current`). The README now lists all of them.

**The CSV and the Parquet must agree on a column NAME.** `Table<>` and
`gen_raw_columns.py` are independent, and the first spelling here was
`src_dev`/`dst_dev` while the generator emits the struct's own
`src_device`/`dst_device`. An assertion written against `src_dev` ran clean
under sqlite and failed under DuckDB with `Referenced column "src_dev" not
found`, on the same capture. Renamed to match; it is the `nccl_comm.rank`
defect that the generator's `RENAMES` table exists to correct, and not creating
it was cheaper than correcting it.

**And on a column TYPE, and on SQL dialect.** Two more that only a two-engine
suite can find: `HAVING` without `GROUP BY` is legal in DuckDB and refused
outright by sqlite; and `params` binds meta.json's integers as INT32, so
`iters * gpus * (gpus-1) * msg_bytes` = 10,066,329,600 **errors** under DuckDB
while sqlite computes it in 64 bits without comment. Both are now spelled
portably, with the measurement in the assertion's own header.

**`sql_check.sh` run by hand skips EVERY derived assertion unless `$PCOMM` is
exported**, and it says nothing about it. `PQ_MODE` is gated on
`$PQ_BIN sql $PQ_DIR "SELECT 1"` succeeding, and `PQ_BIN` defaults to `$PCOMM`,
which only `common.sh` sets. Invoked directly with `PCOMM_BUILD` set but not
`PCOMM`, the mutation test of case 24 reported `PASS (0 goldens, 6 assertions)`
both WITH and WITHOUT the mutation — four assertions silently missing from a
board that looked identical to the good one. That is the failure this suite's
own README gives a heading to, arriving in the harness rather than in a query.
With `PCOMM` exported the same mutation turns `ring_matrix`,
`one_device_per_rank` and `copy_peers_resolve` red, which is the evidence those
three are not vacuous.

**A capture with no MPI must say `PCOMM_RANK=0`.** Nothing else in the suite
meets this, because every other case is launched under mpirun or srun. Without
it the engine falls back to the pid — correct behaviour, and
`suite/asserts/ranks_complete` reports it precisely: rank 0 "silent", rank
1652840 "not launched".

## Findings, written up rather than fixed

**`gpu_device` (528) is absent from every small CUPTI-only capture, and the
mechanism is specific.** GpuDevice is emitted from the arena-flush path — not at
start (the driver may not be loaded) and not at finalize (it answers
`CUDA_ERROR_DEINITIALIZED`, job 45302619) — so it needs a 4096-record buffer to
fill *while the process is alive*. Under CUPTI that does not happen for an
ordinary workload: CUPTI hands activity records over in 8 MB buffers, roughly
80,000 memcpy records to a buffer, so a run producing 4400 of them delivers ALL
of them in a single `on_buffer_complete` from `cuptiActivityFlushAll` **in the
backend's destructor**. The engine's arena then fills for the first time at
exactly the moment the driver has stopped answering.

Reproduction: `copybench 200 4194304 4` under `-p cupti` on du04 — 4400 rows in
534, no 528 stream. `libcuda.so.580.95.05` is mapped in the process throughout
(checked in `/proc/<pid>/maps`), so this is not a missing driver.

Consequence, and it is mild: `_device_key` falls back to its `'O'` ordinal keys,
which its own comment already describes as the legitimate shape of a capture
whose buffers never flushed in-run, and `unresolved_device_copy` stays 0. It
would bite a MULTI-RANK CUPTI capture under `CUDA_VISIBLE_DEVICES` pinning,
where every rank reports ordinal 0 and `(node, ordinal)` is not an identity —
which is the exact failure GpuDevice was added for. Not fixed here: the fix is
an in-run emission point that does not depend on a record buffer filling, which
is a change to the engine's emission policy and belongs in its own step.

## Named precisely: what a capture STILL cannot see

- **Where a copy landed.** `CUpti_ActivityMemcpy6` and
  `CUpti_ActivityMemcpyPtoP4` have no pointer-valued field of any kind, so
  `object_access` cannot place copy traffic in a `mem_object` at either end.
  This is the door's limit, not a scoping choice.
- **Copies in a process that also runs gpumem.** NVBit owns the CUPTI
  subscriber. The capture now SAYS so (`memcpy_capture_unavailable`) instead of
  presenting an empty table, which is the part that was actually missing.
- **`cudaMemset`.** `CUPTI_ACTIVITY_KIND_MEMSET` is a separate activity kind and
  is not enabled. A memset moves no bytes between endpoints, so it is not
  communication; it is named here so its absence is a decision on the record.
- **Copies performed inside a CUDA graph** are reported (the activity records
  carry `graphId`/`graphNodeId`), but the record does not carry those fields, so
  a graph-launched copy cannot be attributed to its graph.
- **The API call itself.** No `CopySource::HostApi` producer exists, so a
  capture cannot say when a copy was *requested*, only when the engine ran it —
  and therefore cannot measure submission-to-execution latency for copies the
  way `GpuKernel.correlation` allows for kernels.

## Follow-ups, in the order they are worth doing

1. **The host-API plane** (`cudaMemcpy*` / `cuMemcpy*` interposition,
   `CopySource::HostApi`). Buys the addresses — and therefore `object_access` —
   and buys copy capture under gpumem. Needs the `cuGetProcAddress` door,
   because nvcc links cudart statically by default.
2. `link_health.memcpy_rows_submission_only`, in the same change as (1).
3. An in-run `GpuDevice` emission point that does not depend on a record buffer
   filling — the finding above. It is latent for now (`ctx_device` supplies
   ownership without it) but it is the record that makes an ORDINAL an identity,
   and a multi-rank CUPTI capture under `CUDA_VISIBLE_DEVICES` pinning needs it:
   every rank reports ordinal 0 there and `(node, ordinal)` stops being a key.
