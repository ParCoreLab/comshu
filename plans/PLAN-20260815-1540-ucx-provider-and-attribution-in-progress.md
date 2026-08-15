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
| MareNostrum 5 | `ssh bsc` / `koc296797@alogin1.bsc.es` | SLURM, multinode H100, NDR IB | **Primary UCX oracle.** OpenMPI was measured loading `libucp`, `libuct`, verbs and mlx5 after a real two-node `MPI_Allreduce` (job 44578355). Build, correctness and attribution gates run here. |
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
