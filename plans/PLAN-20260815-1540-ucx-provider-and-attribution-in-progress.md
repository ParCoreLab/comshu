# PLAN — UCX provider with real cross-layer attribution

- **Created:** 2026-08-15 15:40 +03
- **Status:** IN PROGRESS. The transport gate is settled and the SigmaTrace
  surface has been audited; no pcomm UCX code exists yet.
- **Parent:** `PLAN-20260813-2238-the-gaps-are-in-the-records-not-the-providers-todo.md`,
  Stage B.
- **Goal:** match every useful UCX fact SigmaTrace records, improve the facts it
  records incompletely, and connect UCX intent to the verbs traffic pcomm
  already captures. Do not create an attractive parallel timeline with no
  defensible join.

## The two machines are complementary

| site | access | hardware / role | UCX use in this plan |
|---|---|---|---|
| MareNostrum 5 | `koc296797@alogin2.bsc.es` | SLURM, multinode H100, NDR IB | **Primary UCX oracle.** OpenMPI was measured loading `libucp`, `libuct`, verbs and mlx5 after a real two-node `MPI_Allreduce` (job 44578355). Build, correctness and attribution gates run here. |
| du04 | `ssh missa18@a100`; if MagicDNS fails, `ssh -J mktips@100.64.0.8 missa18@172.20.31.90` | one node, A100 + ConnectX-5, no scheduler; uniquely supports IBGDA | Portability/control run across another UCX, CUDA, rdma-core and NIC generation. It cannot prove multinode UCX or replace MN5. |

The split is load-bearing. du04 is the IBGDA machine; MN5 is the multinode UCX
machine. A green board on either does not subsume the other.

## What SigmaTrace actually has

At `../SigmaTrace` the UCX provider interposes these public UCP doors:

- lifecycle: `ucp_init_version`, worker create/destroy, endpoint create/close;
- traffic: tagged send/receive, put/get and active-message send (`*_nbx`);
- memory: map/unmap;
- progress: `ucp_worker_progress` in full mode.

It records the corresponding events and reader-side endpoint traffic summaries.
That is the minimum parity surface, subject to two corrections:

1. `UcpMemMap` writes `addr=0,size=0` even though the input parameters can carry
   both. pcomm must record the fields selected by `field_mask`, and must not
   invent zero as a measured address or length when they were absent.
2. SigmaTrace defines and reads `UcpEpQpMapping`, but a repository-wide search
   finds no producer. Its reader therefore has a join that its profiler does not
   populate. We copy the requirement, not the missing implementation.

Parity means matching useful semantics, not reproducing these gaps, its debug
`cout`, or its reader's presentation choices.

## The claim pcomm must be able to make

For a controlled MPI operation, pcomm should answer all of these from one
capture:

1. Which UCP context, worker and endpoint existed, with accounted lifetimes?
2. Which UCP operation was issued, with operation kind, local buffer, requested
   bytes and protocol fields (tag/mask, remote address or AM id where relevant)?
3. Was the call accepted, completed immediately, rejected, or represented by a
   request pointer? A returned `ucs_status_ptr_t` is not always a request.
4. Which observed verbs QP(s) carried it, when that relation is measurable?
5. How many wire work requests and bytes resulted, without pretending UCP call
   bytes and wire bytes are the same counter?
6. When the relation is not observable, does the capture say **unresolved** and
   why, rather than guessing or silently attaching the operation to a QP?

The reader may aggregate these facts, but the provider must emit the primitive
records. Summaries are derived data and do not belong on the capture hot path.

## The hard part: operation→QP, not merely endpoint→QP

An endpoint may use several lanes and transports. Even a correct static
`endpoint → one QP` row can therefore be false. The stronger primitive is a
relation from one UCP operation to the verbs work observed beneath it, carrying
the QP number and an attribution method. Endpoint→QP is then a derived set, not
a lossy fact baked into the stream.

The first candidate is a cross-provider call scope in `libpcomm_engine.so`:

```
ucp_*_nbx pre:       push {operation_id, endpoint/worker, kind}
ib_post_send hook:   attach current operation_id to this QP/WR
ucp_*_nbx post:      pop, even on error or exception
```

This works only for verbs work posted synchronously below the UCP call. UCX may
enqueue work and post it later from `ucp_worker_progress`, possibly on another
thread. A thread-local scope is therefore a hypothesis to measure, not the
design to declare complete. A worker-progress scope can establish that UCX
caused a later post but cannot identify the endpoint by itself.

No opaque UCP/UCT structure offsets, debugger-only inspection, or guessed
pointer walks enter the provider. If the public UCP boundary cannot preserve the
operation through deferred progress, that part belongs either in a separately
designed `ucx_deep` provider (reserved IDs 800–899) or remains explicitly
unresolved. Portability is worth more than a mapping that happens to work for
one HPC-X build.

## Stage 0 — establish the oracle before writing records

Create a small MPI/UCX workload matrix on MN5, with fixed payloads and enough
iterations to separate setup from steady state:

- MPI tagged/two-sided traffic (`Send/Recv` and `Allreduce`);
- direct UCP tagged send/receive;
- direct UCP put and get;
- active-message send if the installed UCX exposes the required API;
- immediate completion, deferred request completion, and a deliberate error
  where safely constructible;
- host and CUDA buffers where supported.

For each, run:

1. untraced twice, to establish workload determinism;
2. pcomm ibverbs-only, to establish the existing wire baseline;
3. SigmaTrace UCX-only and UCX+ibverbs, if that checkout builds against MN5's
   UCX;
4. a temporary nested-hook instrument that reports UCP call scope, thread,
   worker/endpoint and every verbs post beneath or after it.

The output is a checked-in parity matrix: hook fires, record counts, requested
bytes, status classes, verbs QPs/WRs/bytes, and the fraction of verbs posts
attributable synchronously. If SigmaTrace cannot build or emits zero mapping
rows, record that as the oracle's measured limit; do not block pcomm on copying
it.

**Exit gate:** the matrix identifies which operations are synchronous, which
escape into progress, and whether one endpoint uses multiple QPs in the tested
workloads. Until then the cross-layer record shape is provisional.

### Measured 2026-08-15 — the public UCP boundary is real and stable

`tests/hpc/probes/ucx_api_coverage` is the first matrix arm: a disposable preload
counter over the same 14 UCP doors SigmaTrace hooks, around a deterministic
two-rank MPI ping-pong (100 sends in each direction, 8192 bytes each) followed
by an all-reduce. It ran twice on two MN5 nodes through `alogin2`:

| job | rank role | tag sends | tag receives | summed raw count sent / received | progress calls |
|---|---:|---:|---:|---:|---:|
| 44629699 | initiator | 133 | 127 | 822148 / 822092 | 329073 |
| 44629699 | responder | 127 | 133 | 822092 / 822148 | 617094 |
| 44629730 | initiator | 133 | 127 | 822148 / 822092 | 510108 |
| 44629730 | responder | 127 | 133 | 822092 / 822148 | 377017 |

Every rank in both runs also had exactly 4 init/cleanup, 3 worker
create/destroy and 5 endpoint create/close calls. Put/get, active-message and
memory-map hooks did not fire in this workload. The lifecycle and tagged-call
surfaces are deterministic; progress-call counts are not and must never be a
golden. `count` above is deliberately labelled raw: UCP datatype semantics mean
it is not universally a byte count, even though this MPI arm's dominant payload
is known independently.

The first submission, 44629663, failed at compile time because MN5's headers do
not indirectly include `<cstdint>` while the local sysroot's do. The probe now
includes what it uses directly. No runtime evidence came from that job.

This settles one gating question: public-symbol interposition is sufficient to
observe the high-level MPI/UCX tagged path on MN5. It does **not** yet settle
operation→QP nesting, direct put/get/AM coverage, request completion, or
multi-lane endpoints; those remain the rest of Stage 0.

### Measured 2026-08-15 — `ibv_post_send` is not UCX's send door on MN5

Job 44635003 ran the same two-rank workload with the public-UCP scope probe and
pcomm's ibverbs provider together. It completed successfully on two MN5 nodes
and reproduced the deterministic UCP lifecycle/tag counts above. Both ranks
reported:

```
[pcomm-ucx-nesting] summary outside=0
```

There were no per-UCP-call buckets because the hooked `ibv_post_send` path did
not execute at all. The capture corroborates this: both ranks contain the
verbs setup, QP/CQ, memory and `ib_post_recv` streams, but neither contains an
`ib_post_send` (type 110) stream. This is not evidence that UCX sent no traffic;
the MPI workload passed. It is evidence that MN5 UCX's mlx5 transport posts its
send WQEs through a direct mlx5/UCT fast path below the public verbs operation.

Therefore the proposed `ucp_*_nbx` → `ibv_post_send` nesting experiment is
settled as the wrong observation point on this transport. It cannot distinguish
synchronous issue from deferred progress and must not be used to declare an
operation resolved or `no_verbs_observed`. The next Stage 0 arm must observe
the actual host-driven mlx5 send/doorbell path and retain the same UCP scope
labels. This is distinct from pcomm's existing mlx5dv doorbell hijack, which is
for DEVX UARs/device-initiated traffic; first identify an exported or safely
patchable host UCT/mlx5 door without relying on opaque structure offsets.

Two preceding submissions produced no attribution evidence: 44630308 selected
`nvc++` and failed to link GCC's `__builtin_ia32_rdtsc`; 44630851 built the
provider but requested the CMake directory target `pcomm` instead of the
executable target `pcomm_launcher`. The checked-in batch script now pins GCC,
uses a job-specific build tree and builds the explicit launcher target.

### Measured 2026-08-16 — Stage 1 public-UCP provider passes on MN5

Job 44675611 built commit `34b4e6b` against MN5's HPC-X UCX 2.22.1 tree and
ran the deterministic two-rank MPI ping-pong under `pcomm -p ucx`. The workload
passed, and both rank captures contained the new provider streams. Their fixed
stream strides give these counts:

| record | rank 0 | rank 1 |
|---|---:|---:|
| `ucx_context` (600) | 4 | 4 |
| `ucx_worker` (602) | 3 | 3 |
| `ucx_endpoint` (604) | 5 | 5 |
| `ucx_tag_send` (606) | 133 | 127 |

The tagged-send counts and rank asymmetry match Stage 0's independent preload
probe (jobs 44629699/44629730), while the capture also contains the engine's
clock, intern and entity-close streams. This proves the initial provider is
co-runnable with the real MPI/UCX path and that its lifecycle accounting is
not merely a compile-time or synthetic-app result. It does not claim operation
completion or operation→QP attribution; those remain Stage 2/3 work.

### Reviewed 2026-08-16 — two lists, and the build that nobody built

The MN5 counts above were re-verified by parsing the capture rather than by
dividing file size by stride: 133/127 `ucx_tag_send`, 5 endpoints, 3 workers,
4 contexts per rank, agreeing exactly. The review found two defects behind them,
both the same shape — a generated list and a hand-written list describing the
same thing, with nothing keeping them in step.

**`pcomm-read` did not compile at all** under `-DPCOMM_DUCKDB=ON`. Its provider
include directories were spelled out by hand (ibverbs, mlx5dv, in two places)
while the generated `raw_columns.hpp` derives its `#include` lines from a GLOB
over the same files, so `providers/ucx` made the generator emit a header no
target could find. It survived because the only consumer of that header is
behind `PCOMM_DUCKDB=ON`: the pre-commit gate builds the core config, which has
no DuckDB, and the MN5 job built only `pcomm_launcher` and `ucx_trace`. **The
configuration that broke is the one no gate builds** — worth remembering next
time a gate is described as green. One glob now feeds both.

**The records were write-only in half the reader.** Parquet and SQL are
generated from that same glob and had the UCX tables immediately; CSV,
`--pftrace` and `pcomm types` come from `TableRecords`, a hand-maintained tuple,
which never got them. A build of the provider's own tree therefore rejected its
own capture with `type 600 is unknown to this build ... a capture from a newer
schema, or a provider this build does not know` — both halves false, and
pointing at a version skew that does not exist. Stage 1's scope above already
listed "reader table ... changes required by the normal record gate"; that item
had not been done, and the gate did not notice because no gate reads a UCX
capture. Now done and verified against job 44675611's capture.

The nesting instrument in `providers/ibverbs/ibverbs_provider.cpp` is **removed**
along with `nesting.sbatch`, its question having been answered by job 44635003
above. The finding moved to the probe's README. `ucx_calls.cpp` keeps
`pcomm_ucx_api_probe_current` for whatever observes the host-side mlx5 send door
in Stage 3.

## Stage 1 — provider vertical slice

Add `providers/ucx/`, built only by `PCOMM_PROVIDER_UCX=ON`, with UCX headers and
library found through the same sysroot/module-aware pattern as other providers.
Add a non-exclusive launcher row (`ucx`, aliases `ucx`/`ucp`) so it co-runs with
ibverbs in one process. The initial slice contains:

- context, worker and endpoint entities plus ordinary `entity_close` records;
- one tagged-send operation record with exact status-pointer classification;
- provider metadata and the reserved 600–699 range declaration;
- schema, conformance, reader table and generated-header changes required by
  the normal record gate;
- a no-UCX core build and a sysroot UCX provider compile test.

Use interned vendor pointers as entity identities, consistent with the rest of
pcomm. Endpoint reuse is guarded by lifecycle; no permanent set may be keyed by
a pointer without deleting it on close.

**Exit gate:** a direct tagged-send case on MN5 emits balanced entities and a
correct operation row while ibverbs in the same pass still emits its unchanged
wire baseline.

## Stage 2 — complete and improve the SigmaTrace surface

Add tagged receive, put, get, active-message send, memory map/unmap and worker
progress. Requirements that are easy to miss:

- obey every UCX `field_mask` before reading an optional field;
- distinguish `UCS_OK`, error pointers and real request pointers;
- capture datatype/count semantics honestly: `count` is not universally bytes,
  so calculate bytes only where the datatype/API contract permits it and retain
  the raw count otherwise;
- a receive call is posted intent, not received completion; do not present its
  requested capacity as delivered wire bytes;
- memory map records the requested/effective address and length when available,
  and relates the resulting `ucp_mem_h` lifetime to `MemObject` without creating
  a second provider-local object identity;
- progress is full-mode telemetry and remains out of simple mode.

Add older non-`nbx` APIs only if Stage 0 shows MN5's MPI/UCX path calls them.
Hook lists follow observed coverage, not API completeness theatre.

**Exit gate:** every useful SigmaTrace UCX event has a pcomm equivalent or a
written, measured reason for exclusion; pcomm carries no known-zero placeholder
where SigmaTrace does.

### Done 2026-08-16 — the surface is complete, the gate is not

All six remaining doors are hooked and all five remaining records written, so
the provider now covers the same 14 public UCP doors SigmaTrace does. Verified
by `nm -D` on the built `.so` rather than read off the version script, and by
`schema_conformance`, where all nine UCX records are byte-identical to
`pcomm.fbs`.

| record | id | notes |
|---|---|---|
| `ucx_tag_recv` | 608 | posted intent; `capacity`, not bytes. Keyed by worker |
| `ucx_rma` | 610 | put and get, one record, `op` discriminates |
| `ucx_am_send` | 612 | header and payload lengths kept apart |
| `ucx_mem_map` | 614 | effective values preferred; `resolved` says which |
| `ucx_worker_progress` | 616 | Full mode only, empty polls dropped at the hook |

`kSchemaHash` 0xD0E6688012222099 -> 0x03A9E43D1D3AC05C. Additive: job 44675611's
capture still decodes all 260 tagged sends, and the committed `ib_pingpong`
fixture's two tests still run rather than skip.

**On the exit gate's own terms this is not finished, and the gap is honest
rather than incidental.** The gate asks for a measured reason for every
exclusion, and two things are excluded without one:

- **Older non-`nbx` APIs.** Stage 0's probe counted the 14 `nbx`-era doors and
  did not count the legacy ones, so "MN5's MPI does not call them" is an
  assumption, not a measurement. It is a cheap one to settle — one probe arm —
  and until it is settled the surface is complete only with respect to the
  doors we thought to look at.
- **Put, get and AM fired zero times** in the Stage 0 arm. They are hooked on
  the argument that this was a fact about a tagged ping-pong workload rather
  than about the API, which is sound reasoning and still not evidence. Nothing
  has yet observed a single `ucx_rma` or `ucx_am_send` row on real hardware, so
  those three doors are compile-tested only.

Neither blocks Stage 3, and both belong in the same MN5 run: an RMA/AM workload
would exercise the three unproven doors and settle the legacy-API question in
one allocation. **Until then no UCX record beyond the tagged path and the
lifecycle has been seen on real hardware**, and the provider has no regression
gate of any kind — Stage 4 remains entirely unstarted.

### Measured 2026-08-16 — the door sweep, and why matching SigmaTrace was wrong

The legacy-API question turned out to be answerable **statically, from the login
node, with no allocation at all**: if a component calls `ucp_*`, that is an UND
reference in its `.so`, and every library below was already measured to be
loaded on the MPI path (job 44578355). This is weaker than a probe run and far
stronger than "the API exists".

| component | doors referenced |
|---|---|
| `mca_pml_ucx.so` (MPI p2p) | `tag_send_nb`, `tag_send_sync_nb`, `tag_recv_nb`, `tag_msg_recv_nb`, **and** the `_nbx` pair |
| `mca_osc_ucx.so` (MPI one-sided) | `put_nbi`, `get_nbi`, `atomic_post`, `atomic_fetch_nb` — **no `_nbx` RMA at all** |
| `mca_spml_ucx.so` (OpenSHMEM) | `put_nbx`, `get_nbx`, `ep_destroy` |
| `hmca_bcol_ucx_p2p.so` (hcoll) | `tag_send_nb`, `tag_recv_nb`, `ep_close_nb` |
| `libsmx-3.10.so` (SHARP) | `tag_send_nb`, `tag_msg_recv_nb` |
| `libnccl-net.so` (NCCL plugin) | `am_send_nb`, `get_nb`, `put_nbi`, and `_nbx` tagged |
| `libucc_tl_ucp.so` (UCC) | the `_nbx` set — already covered |

**Copying SigmaTrace's door list was the wrong target.** It is a list of the
modern API, and MN5's stack is substantially not on the modern API. Two things
followed, and the first was a live bug rather than missing coverage:

- **`ucx_endpoint` leaked on three of its four close doors.** Only
  `ucp_ep_close_nbx` was hooked. An entity opened and never closed stays live
  forever, and the allocator reuses the address — the exact defect
  `ibv_close_device` exists to prevent. Fixed.
- **MPI RMA would have reported a confident zero.** `mca_osc_ucx` touches none
  of the doors we hooked, so `MPI_Put`/`MPI_Get` traffic would have produced an
  empty `ucx_rma` table indistinguishable from a run that did no one-sided
  traffic at all. Fixed.

Nine doors added (23 total). The three operation records gained a `door` byte so
a capture states which entry points an installation actually drives instead of
requiring this sweep to recover it, and so `ucp_tag_send_sync_nb` — MPI_Ssend,
which completes only on receiver match — stays distinguishable from an ordinary
buffered send.

**Both remaining items are now closed** — see the next section.

The runtime gap is unchanged and is now the binding one: **still nothing but the
tagged path and lifecycle has been observed on real hardware.** The sweep proves
the code paths exist; only a run proves which are taken. One two-node job with an
MPI RMA + `MPI_Ssend` workload would exercise the `_nbi` doors, the legacy tagged
doors and the extra close doors at once, and is the obvious next step.

## Stage 3 — cross-layer attribution

Implement the least invasive mechanism Stage 0 proves:

- synchronous nested posts: engine-owned operation scope plus an additive edge
  emitted by the verbs provider;
- deferred same-thread posts: extend the scope only across a measured safe
  boundary, never across arbitrary user execution;
- deferred/cross-thread posts: either a public-API-derived request/worker
  correlation with explicit lifecycle, or unresolved; do not infer from nearest
  timestamp;
- multi-lane endpoints: emit all observed operation→QP edges.

The edge includes an attribution-method enum so readers and assertions can
separate exact nesting from any later mechanism. A completeness artifact or
health view reports `attributed`, `unresolved_deferred`, `no_verbs_observed`,
and `non_verbs_transport`; absence is never silently read as success.

**Exit gate:** on the controlled matrix, every attribution claimed exact agrees
with the nested trace, every observed QP exists in `ib_queue_pair`, and totals
never double-count UCX intent as additional wire traffic.

## Stage 4 — parity and regression gates on real machines

MN5 is the merge gate:

- add a two-node UCX suite case with deterministic operation counts and payloads;
- assert entity close accounting, operation/status counts, requested-byte
  totals, coexistence with ibverbs, referential integrity of every attribution
  edge, and explicit unresolved counts;
- run twice and compare the stable surfaces before freezing a golden;
- compare against SigmaTrace on the same binary in separate runs. Two preload
  stacks need not coexist for parity, and forcing them together adds a different
  experiment;
- run the existing MN5 board to prove the provider is additive when unselected
  and does not perturb ibverbs when selected alongside it.

Then run the direct-UCP subset on du04. This is a portability gate across A100,
ConnectX-5 and its UCX/rdma-core version. It is not allowed to weaken or replace
the MN5 multinode assertions, and UCX work must not disturb du04's existing
IBGDA board.

## Definition of done

- `pcomm list` exposes a built UCX provider and `pcomm run -p ucx,ib` produces
  one shared-pass capture.
- All useful SigmaTrace UCX lifecycle, traffic, memory and progress semantics
  are present; known SigmaTrace gaps are fixed rather than copied.
- The capture relates UCX operations to every verbs QP it can prove, supports
  multi-QP endpoints, and quantifies unresolved operations.
- High-level requested bytes and low-level wire bytes remain separate measures.
- The direct matrix and real MPI case pass twice on MN5; the portable subset
  passes on du04; all pre-existing boards remain green.
- The plan records actual job IDs, versions, counts and any remaining blind
  spots, then is renamed `-done.md` only after those measurements exist.

## Non-goals

- Reimplementing ucTrace/deep UCT internals in this provider. IDs 800–899 remain
  reserved for that distinct project.
- Pretending every MPI operation maps one-to-one to a UCP call or one verbs WR.
- Using timestamps alone as causal attribution.
- Emitting SigmaTrace-compatible binary records. Semantic parity is the goal;
  pcomm's four-archetype schema and reader remain its own.

### Measured 2026-08-16 — UCX runs locally, and that changes the gate story

The two items left open above are done, and the way the second was settled
matters more than the record it produced.

**`ucp_tag_msg_recv_nb`, solved by hooking the probe.** That door carries a
`ucp_tag_message_h` and no tag, so its row could not be keyed without inventing
one. But the tag is not lost: `ucp_tag_probe_nb` was handed it, in
`ucp_tag_recv_info_t`, when it produced the handle. So the probe is hooked as
the tag's source (`ucx_tag_probe`, 618), `UcxTagRecv` gains a `message` handle,
and the reader joins them. `tag`/`tag_mask` stay 0 on that path because that
door genuinely has neither — 0 is honest where a guess would read exactly like
a measurement.

The join is **temporal, not equality**: UCX recycles the handle out of its
request mpool, which it announced itself during the validating run.

`ucx_tag_probe.length` is a bonus worth naming: it is the one TRUE byte count
anywhere in the UCX family. Every other size is capacity offered or a raw
datatype count.

**Atomics (`ucx_atomic`, 620)**, with the opcode normalised. UCX numbers the
same operations differently in each of its three atomic doors — `op = 1` is AND
via `atomic_post`, SWAP via `atomic_fetch_nb`, SWAP via `atomic_op_nbx` — so the
raw byte is not merely opaque, it is ambiguous, and is mapped at the hook.

### The thing that should have been obvious a week ago

**UCX needs no cluster.** The self transport runs the whole thing in one
process, and `ucp_init` succeeds on an ordinary workstation with nothing but the
rdma-core/ucx development packages. Every claim this provider has made until now
was validated by submitting a two-node MN5 job and reading its output by hand —
an allocation, a queue wait and a reservation to discover that a record was
missing.

`tests/ucx_selftest.cpp` + `tests/ucx_records.sh` is the provider's first
regression gate, and it runs in 0.08 s in `ctest`.

**It found a live bug on its first run.** UCX implements its legacy doors on top
of the modern ones, so with global interposers a single `ucp_tag_send_nb`
produced a `nb` row AND an `nbx` row — three sends recorded as five, receives
likewise. This is `ibv_reg_mr` over `ibv_reg_mr_iova2` again (ib_hooks.hpp), and
takes the same remedy: a thread-local depth counter with the outer door as the
recorder. Without a local test it would have shipped as a silent 2x inflation on
every legacy send, and the MN5 runs would not have caught it — nothing there
counts sends independently.

The gate was then verified to FAIL when the guard is removed (4 sends become 6),
because a green test nobody has seen fail is not evidence. Per-door counts are
exact equality for the same reason: `>=` passes happily on a doubling.

**What this does not cover.** The self transport is not the wire: no `_nbi` RMA,
no atomics, no multi-rank endpoints, and no mlx5. So the local gate defends the
record layer and the door accounting, and MN5 is still the only place the
transport-facing half can be exercised. The right next run is one two-node job
with MPI RMA and `MPI_Ssend`, which would exercise the `_nbi` doors, the legacy
tagged doors, the atomics and the extra close doors at once — and it can now be
written against a provider whose record layer is already known good.
