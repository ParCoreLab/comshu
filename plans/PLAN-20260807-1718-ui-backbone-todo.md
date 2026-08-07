# PLAN — the UI is a query surface over derived tables, not a viewer of captures

- **Created:** 2026-08-07 17:18 +03
- **Status:** TODO — direction agreed in discussion, nothing started. Step 0 is a
  hard prerequisite for every other step and is the only one that can begin
  today.
- **Depends on:** `PLAN-20260729-1830-record-format-flatbuffers-todo.md` for the
  Parquet writer, which is this plan's input contract;
  `PLAN-20260806-1329-generic-memory-attribution-todo.md` for the object views
  (5, 7) to have anything to show on ibverbs-only captures.
- **Related:** `PLAN-20260803-2253-ibgda-device-initiated-capture-in-progress.md`
  — under IBGDA the `ib_*` streams are empty and `comm_edges` is therefore
  *blank*, which is the single largest hole in the derived layer today.

## The claim this plan rests on

Every view we want is a **small aggregate of a huge input**, with exactly one
exception.

| view | rows the client draws |
|---|---|
| communication matrix | N² cells, N = ranks. Kilobytes. |
| dendrogram over comm patterns | a linkage tree over the same N. Kilobytes. |
| sankey (flow / opcode breakdown) | a groupby over 2–3 categorical dims. Kilobytes. |
| source-line attribution | one row per `(file,line)`. Thousands. |
| object view | one row per merged `MemObject`. Thousands. |
| GPU view | N rows. |
| chat / NL query | whatever a capped SQL result returns. |
| **timeline** | **millions of `IbPostSend` / `GpuAccess` rows.** |

The timeline is delegated to Perfetto, which we already export to
(`--perfetto`, `--pftrace`). With it removed, **no client ever renders more than
a few thousand marks.**

Two consequences, and both are load-bearing:

1. **The frontend has no performance problem.** The expensive work is a groupby
   over a memory-mapped fixed-stride array, and that is `pcomm-read`'s job
   whatever draws the pixels. This is why the plan does not reach for a native
   immediate-mode toolkit — see *Rejected* below.
2. **The risk is not rendering. It is the derived tables.** The framework choice
   is reversible in an afternoon; the semantics of "rank 3 sent 4.2 GB to rank
   11" are not, and getting them wrong has already cost this project once
   (`qp_num` collisions, below).

## What already exists, and what the gap actually is

The derived layer is not empty. `pcomm-read`'s `CsvAgg` already computes, in a
single pass inside `export_csv`:

- **`comm_edges`** — `(src_rank, dst_rank, sends, bytes)`, by resolving each
  QP's RTR `dest_qp_num` through a `(lid, qp_num) -> rank` owner map. The
  comment at `pcomm-read/main.cpp:468` is the most important prior art in this
  plan: keying that map on `qp_num` alone produced impossible self-edges and a
  4-peer set for an 8-rank job, because `qp_num` is HCA-local. **The pair is the
  key.** Any new cross-rank join must assume the same about every wire id it
  touches.
- **`mem_owner`** — address → object by range containment, *scoped to the rank*,
  with `ambiguous_refs` / `nested_refs` / `devices` reported rather than
  silently resolved.
- **`RankClock` / `build_clocks`** — ticks → shared wall-ns origin from
  `ClockAnchor`, used by both Perfetto exporters.

So the shape is right and the hard-won lessons are already encoded. What is
missing is that these are **inline accumulators inside one CSV exporter**, not a
named stage with its own output. Specifically:

- `comm_edges` is **blank on GPU-initiated runs**. It is fed only by
  `IbPostSend` + `IbModifyQp`. Under IBGDA the topology lives in
  `Mlx5DevxQpConnect` (which carries `src_lid`, `dest_lid`, `dest_qp_num` for
  exactly this reason) and the data plane is not in the capture at all.
- `comm_edges` has **no time and no size distribution** — one collapsed total
  per pair. Sufficient for the matrix; insufficient for the sankey, for a
  time-brushed matrix, or for "what changed in phase 2".
- `RankClock` is computed **inside the exporters**, so no table carries aligned
  wall-ns. Every consumer re-derives it.
- There is no per-`(file,line)` and no per-object rollup; the source-line and
  object views have no table to read.

**This plan's core work is promoting that layer to a first-class pass and
finishing it.** The frontend is the cheap half.

## Design decisions

### The UI never reads `.pcs`

Parquet is the contract. The reader owns stream headers, `kSchemaHash`
validation, stride checks, `version_map`, and intern resolution — deliberately
concentrated in one place, and the frontend must not inherit any of it. It also
means users get Polars/pandas/DuckDB access to the same files for free, which
the CSVs already demonstrate demand for.

### DuckDB, not a hand-written query API

Decided primarily by the chat view. With SQL, a natural-language question is:
model sees the schema, emits a query, we execute it read-only under a row cap
and a timeout. Without SQL, every question a user might ask needs a bespoke
endpoint — unbounded work with no stopping point. DuckDB also reads our Parquet
natively, so the derived tables need no second serialization.

### One binary: `pcomm ui <trace-dir>`

DuckDB linked in, the built frontend embedded as static assets, serving
`localhost:PORT`. The pprof / Perfetto / Jupyter shape.

Chosen over a pure static DuckDB-WASM page **because of MN5 specifically**:
captures live in `$SCRATCH` on a machine we reach over SSH, and `pcomm ui` plus
a port-forward is the only workflow that does not involve dragging gigabytes
home. It degrades to the WASM shape later without a rewrite — provided the
frontend routes *every* data access through one `query(sql) -> rows` module and
nothing else touches the network. That seam is a rule from day one, not a
refactor.

### Svelte 5 + Tailwind, with Vega / Vega-Lite for marks

- **Vega-Lite for the chat path.** The model emits a *spec*, never JavaScript.
  Generated JS executing in our page is a footgun with no upside here, and a
  declarative spec is inspectable and cheap to validate before render.
- **Vega for the curated figures**, where its grammar reaches.
- **d3 directly where it does not.** Do not contort a spec to avoid dropping to
  d3 — the fallback is the plan, not a failure of it.

**Both source trees are vendored under `reference/`** — `vega-6.3.0` (the lerna
monorepo, one package per transform family) and `vega-lite-6.4.3` (`src/`,
`examples/`, `site/`). Read the transform source before concluding a figure is
out of reach; the first such conclusion was already wrong.

Namely, the dendrogram: `packages/vega-hierarchy` ports d3-hierarchy, and the
`tree` transform's `method: "cluster"` *is* the dendrogram layout — leaves
aligned at equal depth, cartesian or polar. So it is reachable declaratively and
should be attempted that way first.

That leaves two places where the fallback is still likely, and they are narrower
than "grammars are bad at figures":

- **Sankey.** There is no sankey or flow transform in `vega-transforms` or
  `vega-hierarchy`. Either a custom transform or a layout precomputed in the
  reader and fed in as data — the latter is more in keeping with the rest of
  this plan.
- **The dendrogram↔matrix composite.** The tree is fine; the hard part is
  propagating the clustered leaf ordering into the matrix axes so the two views
  stay locked. Vega can express it through a shared derived dataset, but that is
  where the spec stops being small enough to be worth the abstraction.

The split is deliberate: one rendering path we control precisely for the eight
curated views, one constrained path the model is allowed to drive.

### Timeline stays Perfetto

`--pftrace` already produces a native trace. v1 is "export, then upload to
ui.perfetto.dev". **Do not build a timeline engine.** The future upgrade, worth
knowing exists before designing cross-linking: Perfetto's UI accepts a trace
over `postMessage` into an iframe, so clicking a QP in our matrix can open the
timeline scrolled to that QP's slices with no timeline code of ours.

## Steps

### Step 0 — a capture corpus, before any of the rest (**prerequisite**)

Nothing below can be designed against `tests/data/ib_pingpong-44368973` (72 KB,
2 ranks). We need a checked-in-or-archived set of **real** captures spanning the
axes that break things:

- **scale** — 2 ranks / 8 ranks / a genuinely multinode job (≥32 ranks). The
  `qp_num` collision only appeared at 8; assume the next such bug needs 32.
- **provider mix** — ibverbs-only; gpumem-only; both (case 06, the coexistence
  case); **IBGDA/DEVX-only**, where `ib_*` is empty and `comm_edges` is blank.
- **workload shape** — pingpong (trivial known-good topology), NCCL allreduce
  (ring/tree — the dendrogram should *find* that structure, which is how we know
  the dendrogram works), NVSHMEM (symmetric heap, case 07).
- **volume** — at least one capture large enough to be uncomfortable, so the
  Parquet and query layers are sized against reality rather than against
  pingpong.

Record for each: rank count, node count, job id, provider set, raw `.pcs` bytes
per stream, and record counts per type. **That table is the sizing input for
every decision below** — in particular whether the raw event tables need
partitioning by rank and whether the linker must become streaming.

`tests/hpc/suite` already produces most of these on MN5. The work is running
them at size and *keeping the outputs*, not writing new cases.

Acceptance: a corpus directory, an index listing the above, and the honest
answer to "how many `IbPostSend` rows does a 64-rank NCCL allreduce produce".

### Step 1 — Parquet, as already planned

The flatbuffers plan's stated target. No new design here; this step exists only
to state that it blocks steps 2+ and that the schema is the CSV schema — the
`Table<R>` specializations already define every column exactly once, and the
Parquet writer must be derived from that same list, not written in parallel.
`--dump` and `--csv` drifting apart is the precedent.

Acceptance: `--parquet` output reproduces the CSV goldens through a DuckDB
query, and adding a record type still means one `Table<>` entry.

### Step 2 — promote the derived layer to `pcomm-read --link`

Lift `CsvAgg` out of `export_csv` into a named pass with its own Parquet
outputs. Not a refactor for tidiness: the current accumulators run only when
someone asks for CSV, and their results are unavailable to any other exporter.

Tables to emit:

- `rank` — rank, hostname, device set, LID(s), clock anchor coefficients.
- `comm_edges` — extended: `(src_rank, dst_rank, time_bucket, size_class, sends,
  bytes)`. The extra two dimensions are what the sankey and the time-brushed
  matrix read; collapsing them at write time is what makes those views
  impossible. Bucket width chosen from step 0's numbers.
- `endpoint` — `(rank, lid, qp_num, qp_type, peer_rank, transport)`, unifying
  the ibverbs and DEVX paths so consumers never branch on provider.
- `object` — the merged `mem_owner` rollup, with `devices > 1` preserved as the
  disagreement flag it already is.
- `source_rollup` — `(file, line, accesses, bytes, distinct_objects)`.
- `event_time` — the aligned-wall-ns column, computed once here rather than in
  each exporter.

**Close the DEVX hole in this step, not later.** `Mlx5DevxQpConnect` carries
`src_lid`/`dest_lid`/`dest_qp_num`; feed the same owner map from it so
`comm_edges` is populated on GPU-initiated runs. A comm matrix that is blank for
the exact workloads the IBGDA work exists to capture is not shippable, and
discovering that after the frontend is built means rebuilding the frontend's
assumptions too.

Acceptance, as derived assertions rather than goldens: on the corpus's NCCL
capture, `comm_edges` has no self-edges and no `dst_rank = -1` rows above a
stated threshold; on the IBGDA capture it is non-empty; peer sets are symmetric
where the workload is symmetric.

### Step 3 — `pcomm ui`, one panel

Server + embedded Svelte app + the **communication matrix only**. One panel, end
to end, over a real corpus capture.

The point is to prove the whole spine — Parquet → DuckDB → `query()` → Vega —
before eight panels are riding on it. Second panel only after the first is
genuinely good.

Acceptance: `pcomm ui <corpus/nccl-64>` renders a correct 64×64 matrix, and the
numbers match a DuckDB query run by hand against `comm_edges.parquet`.

### Step 4 — the remaining curated panels

Dendrogram, sankey, object view, GPU view, source view — in that order, because
that is roughly increasing dependency on tables that may not exist yet. The
source view is last: it needs `SourceLine`, which only gpumem produces today.

Each panel is a SQL query plus a spec (or a d3 module). If a panel needs a new
table, it goes in step 2's pass — **never a bespoke aggregation in the server**.
That is how the derived layer forks.

### Step 5 — the chat panel

Model sees: the derived-table schema, a short catalog describing what each table
*means*, and a handful of worked example queries. Constraints, all of them
non-negotiable:

- read-only connection, hard row cap, hard timeout;
- the model never sees raw rows, only schema and its own results;
- output is either a table or a **Vega-Lite spec**, validated before render.

### Step 6 — `--bundle` (opportunistic)

A single self-contained HTML file with the derived aggregates inlined, for
sharing a result or attaching to a paper's artifact submission. Cheap once step
4 exists, since the aggregates are already kilobytes, and it is a real
artifact-evaluation story for the SC26 work.

## Risks

- **Step 0 gets skipped** because pingpong "works". Then the linker is designed
  against 2 ranks and the `qp_num`-collision class of bug is rediscovered at
  demo time. This is the most likely way this plan fails.
- **Panels grow private aggregations in the server.** The derived layer forks,
  two panels disagree about total bytes, and there is no single place to fix it.
  Same failure as `--dump` vs `--csv`, one layer up.
- **The WASM seam rots.** If any component fetches anything other than through
  `query()`, the static/shareable mode quietly stops being reachable. Worth a
  lint rule rather than a good intention.
- **`comm_edges` accuracy at multinode scale.** `main.cpp:750` already flags
  that the owner map is exact for one HCA and approximate across nodes until the
  local LID is captured directly. The matrix is the UI's headline figure, so
  this stops being a footnote the moment it is drawn — it must be either fixed
  or *visibly* qualified in the UI. A wrong matrix that looks confident is worse
  than no matrix.
- **Perfetto delegation reads as a gap.** Accept it. Owning a timeline engine
  costs more than every other panel combined.

## Rejected

- **ImGui / a native immediate-mode UI.** Its one advantage is drawing a million
  marks, which is precisely the case we hand to Perfetto. Against that: no
  sankey, no dendrogram, painful text layout for a source viewer with linked
  gutters, a screenshot instead of a URL to share, and no reuse of existing
  Svelte/d3 experience. If a future panel genuinely needs a million interactive
  points, that panel is a Perfetto plugin, not a reason to move the whole UI.
- **Streamlit, again.** Prior art in this project; ruled out on rerun semantics
  and unmanageable layout control.
- **Electron/Tauri desktop app.** Packaging cost with no advantage over
  `pcomm ui` + port-forward, which is the workflow that actually matters here.
- **Pure static DuckDB-WASM as the primary mode.** Cannot reach a trace sitting
  in `$SCRATCH`. Kept as the *secondary* mode (step 6), which is why the
  `query()` seam exists.
- **The UI reading `.pcs` directly.** Would duplicate version mapping, schema
  hashing, and intern resolution outside the one place that owns them.
- **Generated JavaScript from the chat panel.** A spec is inspectable and
  validatable; arbitrary JS in our page is not.

## Explicitly out of scope

- A timeline view of our own.
- Live/streaming capture into the UI. Everything here is post-mortem over a
  finished trace directory.
- Multi-trace comparison (run A vs run B). Wanted eventually; it changes the
  derived-table keys, so it must not be retrofitted casually.
- Authentication / multi-user hosting. `localhost` and a port-forward.
