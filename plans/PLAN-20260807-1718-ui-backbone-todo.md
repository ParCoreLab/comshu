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

#### Done 2026-08-09 — tooling, corpus, and what it measured

`tests/hpc/corpus/` holds `harvest.sh`, `index.sh`, the committed `index.tsv` /
`types.tsv`, and `scale_nccl.sbatch`. Captures live in `~/waste/pcomm/corpus`
(`PCOMM_CORPUS`) — outside the repo because they are hundreds of MB, and outside
`~/pockt` because that tree is syncthing-mirrored and the corpus is bulky,
local, and regenerable. Eight captures harvested from MN5 job 44400747, 400 MB.

Four findings, each of which changes something below:

1. **The 20 GB of historical captures on MN5 are readable under HEAD.** They
   carry schema `14b1d68c503ae656` against HEAD's `160d1b71ce3bed36`, and
   `stream_reader.hpp` refuses only on a *stride* disagreement, warning on a hash
   one. `nccl_internode-44400747` exports its full CSV set with nothing but
   per-file notes. The corpus did not need re-capturing, and step 1 does not
   either.

2. **`gpu_access` is the firehose, not `ib_post_send` — by 6×.** Per rank:
   294,987 (`nccl_p2p`) and 196,608 (`nvshmem`) gpu-accesses, against 49,235
   sends per rank at the worst (`nvshmem_multinode`). Both raw-event tables need
   the partitioning treatment, but `gpu_access` is the one that decides whether
   the linker must stream. The plan's framing of `IbPostSend` as *the* firehose
   was wrong; it is the second one.

3. **`comm_edges` is correct at 8 ranks, on two different workload shapes.**
   `nccl_internode-44400747` yields a clean ring: 1.17 GB forward to `(r+1)%8`,
   71,680 B of acks back to `(r-1)%8`, no self-edges, no `dst_rank = -1`.
   `nvshmem_multinode-44400747` independently agrees with its case README —
   exactly the two node-crossing hops (3→4, 7→0) carry 196,620 sends / 787,200 B
   and everything else is bootstrap chatter. So the matrix panel (step 3) has a
   correct data source *today* and does not have to wait on step 2, and the NCCL
   ring is the oracle the dendrogram must recover in step 4.

   **But that second matrix is dense and spans five orders of magnitude** — 44
   of 56 possible off-diagonal cells are populated, two of them at 196,620 and
   the rest around 14. A linear color scale renders it as two lit cells on a
   dead grid. The matrix panel needs a log or rank-order scale and a legend that
   admits which it is using; "sum of bytes, linearly encoded" is not a viable
   default. This is precisely the kind of thing the corpus existed to find, and
   it would not have appeared in pingpong.

4. **A capture directory carries no provenance whatsoever** — it is
   `rank-N/<type_id>.pcs` and nothing else. Everything known about a capture
   lives in the name of the directory it was written into, which a copy destroys.
   Hence `provenance.tsv`, written at harvest time. For retroactive harvests the
   tracer commit is genuinely unrecoverable and is recorded as `unknown` rather
   than guessed from the job date.

Supporting change: `pcomm-read --types` emits the `type_id -> table name` map
from the same `Table<>` list every exporter derives from, because the index (and
later the UI) would otherwise keep a private copy of that mapping.

5. **The 32-rank scale capture: `comm_edges` is exact across 8 nodes.** Job
   44445029, `scale_nccl-44445029` in the corpus — case 02's workload with one
   variable changed, 2 nodes to 8. COMPLETED in 3m19s, 619,131 records, 49 MB.
   `edges=64 self_edges=0 unresolved=0`: exactly 32 forward and 32 back, a
   perfect ring.

   **This retires a documented risk.** `pcomm-read/main.cpp:750` and the suite
   README both say the `(lid, qp_num)` owner map is "exact for a single HCA and
   approximate at multinode scale". At 32 ranks over 8 nodes it is exact — not
   approximately, but with zero unresolved and zero self-edges. The caveat was
   written when the key was `qp_num` alone; adding the LID evidently fixed it,
   and nothing had measured that until now. Both comments should be corrected
   rather than left to frighten the next reader. (It remains untested above 32.)

6. **Per-rank volume is not constant, so linear extrapolation was wrong.** NCCL
   sends per rank went 3,360 at 8 ranks to 7,440 at 32 — total sends 26,880 to
   238,080, i.e. 8.9× for a 4× rank increase. My own projection in the first
   draft of this step scaled per-rank counts linearly and was wrong for exactly
   the reason the step existed to catch. A 64-rank NCCL run of this shape lands
   near ~0.7-1M sends, not the "low tens of millions" a linear guess produced.

   The number the acceptance asked for still cannot be *measured* on this QoS:
   `acc_debug` caps at `node=8` (MaxWall 02:00:00, MaxSubmitPU 1), so 32 ranks
   is the largest capture reachable there at all and 64 needs `acc_ehpc`. But
   the sizing question is answered well enough to proceed: **nothing in the
   corpus is large enough to threaten the design.** The biggest capture is 167 MB
   / 2.1M records. Parquet and DuckDB are not troubled by that, so step 1 does
   not need partitioning-by-rank on day one — it needs to not *foreclose* it.

### Step 1 — Parquet — **DONE 2026-08-09**

Landed as `pcomm-read --parquet <outdir>`, plus `--sql <export-dir> "<query>"`.

**DuckDB, not Arrow.** It writes Parquet natively, ships as one prebuilt library
with no dependencies, and is the same engine steps 3 and 5 query with — one
dependency covering writer, query layer and UI server. `tools/get_duckdb.sh`
fetches the pinned v1.5.5 into `.tools/duckdb` (gitignored, as flatc is).

**`PCOMM_DUCKDB` defaults to OFF, and that default is load-bearing.** MN5 has no
network and no `cmake` outside `module load` — verified, not inherited from
`get_flatc.sh`'s comment — and every suite case runs `pcomm-read --csv` there. A
mandatory DuckDB would break the cluster path the whole suite depends on.
Parquet is produced where the UI runs, which is not the cluster. `--csv` output
is byte-identical across all 27 tables, so no golden moves.

**Raw records with native types, and the enrichment stays in SQL.** The Parquet
tables are the structs as they are on disk: one column per field, the field's
own type, plus `rank` from the directory. No decoded opcode names, no resolved
intern handles — those are semantic and belong to the derived layer, where SQL
expresses them better than a C++ printer. `--csv` keeps the decoded columns.

The column tables are **generated** from the record declarations at build time
(`tools/gen_raw_columns.py`, reusing `schema_check.py`'s parser), so a raw
column cannot describe a field differently from the struct it came from, and a
new record type appears automatically. Build-time only and never committed, so
the stale-artifact cost is not paid. This is *not* the FlatBuffers reflection
path rejected in gap 7 of `PLAN-20260730-1900`: nothing links the FlatBuffers
runtime, and no semantics are claimed.

#### The detour worth recording

The first implementation staged through CSV and let DuckDB infer types from the
text. It was wrong, and silently: `mem_object.base` (a `uint64_t`) came back as
the integer `32825344` while `ib_device.node_guid` stayed the *string*
`"0x946dae030047bf46"` — same hex notation, different inferred type, decided by
whether the value happened to exceed int64. A column's type would depend on
which capture you opened, which is fatal for a contract the UI writes queries
against.

The cause was reaching for `Table<R>::row()`, the only per-record description
that existed, which *prints* a record and offers no way to ask for a field's
value. The fix was not to add a parallel type declaration — that is the drift
`Table<>` exists to prevent — but to stop laundering typed data through text.
`load_stream` already returns `Stamped<R>` with real C++ types; those go to the
appender as themselves. `base` is now `UBIGINT` because the struct says so, on
every capture.

Measured on the corpus: all 9 captures export with zero failures; the 2.1M-row
`nvshmem_multinode` takes 0.59 s and compresses 167 MB of `.pcs` to 27 MB of
Parquet; row counts match the stream inventory exactly for every table,
including the two intern streams that `--csv` does not export at all. Peak
memory is DuckDB's materialised table (~630 MB there), not the reader's buffers
— a `temp_directory` is set so a much larger capture gets slower rather than
killed, though nothing in the corpus is big enough to make it spill.

### Step 1 (original text) — Parquet, as already planned

The flatbuffers plan's stated target. No new design here; this step exists only
to state that it blocks steps 2+ and that the schema is the CSV schema — the
`Table<R>` specializations already define every column exactly once, and the
Parquet writer must be derived from that same list, not written in parallel.
`--dump` and `--csv` drifting apart is the precedent.

Acceptance: `--parquet` output reproduces the CSV goldens through a DuckDB
query, and adding a record type still means one `Table<>` entry.

### Step 2 — **PARTLY DONE 2026-08-09** — `pcomm-read --link`

Landed as `pcomm-read --link <export-dir>`: reads the raw Parquet, writes the
derived tables beside it. Three so far — `endpoint`, `comm_edges`,
`link_health` — with `rank`, `object`, `source_rollup` and `event_time` still to
come, and `comm_edges` not yet carrying the `time_bucket` / `size_class`
dimensions the sankey needs.

**SQL, not C++ accumulators.** This reverses the plan's assumption that `CsvAgg`
would be lifted into a named pass. Once step 1 made the raw tables typed and
complete, the derived layer became a join — which SQL states directly, the UI
queries through DuckDB anyway, and a reader can run by hand. The queries live in
`pcomm-read/link_sql.hpp`. `CsvAgg` **stays**: the suite's goldens are written
against it, and it is the oracle the SQL is verified against.

**Verified against that oracle on all 9 corpus captures: `comm_edges` is
byte-identical to the C++ implementation, and `link_health` is zero everywhere.**
That includes the three gpumem-only captures, where every `ib_*` stream is
absent and the queries must still run.

**The DEVX hole is closed.** `_owner`, `_rtr` and `_sends` union the verbs and
DEVX planes, so `Mlx5DevxQpConnect` supplies topology and `Mlx5DeviceSend`
supplies traffic — the union its own declaration anticipated ("the same shape …
so a reader can union the two"). A consumer of `comm_edges` never learns which
transport ran, which is the point.

No corpus capture has DEVX (cases 09-12 are du04-only), so this was verified
against a **fabricated IBGDA-shaped export** — two ranks, zero `ib_*` tables —
which resolves to a correct two-edge graph with `transport = devx`. That is a
weaker check than a real capture and should be re-run against one when a du04
capture reaches the corpus. It is, however, much better than shipping the union
unexercised, which is the failure mode this repo already names.

Supporting decisions:

- **A missing stream becomes an empty table, not a missing one.** The queries
  reference both transports, so they name tables no single-provider capture has.
  The empty table is built from the same generated DDL the exporter writes with,
  so the columns are right; the exporter still writes no file, so "absent" and
  "empty" stay distinguishable where that matters.
- **`--link` accepts a CSV export as well as Parquet**, loading it through the
  generated DDL and `COPY FROM` rather than `read_csv`'s auto-detect — the types
  stay declared rather than inferred, for the reason step 1 records.
- **`link_health` is a table, not a log line.** `colliding_endpoints`,
  `unresolved_sends`, `self_edges` — all expected zero, all printed to stderr
  when not. The UI is meant to surface it rather than render a confident matrix
  over a hole.

Two bugs found and fixed while testing, both in step 1's code:

- `--sql` on a missing directory **aborted** with an uncaught
  `filesystem_error`, because `directory_iterator` throws.
- `--sql` wrote results with `COPY ... TO '/dev/stdout'`, which **seeks to
  offset 0 when stdout is a redirected file** and silently overwrote everything
  the process had already written. It now goes via a temp file. This one is
  worth remembering: the tidier-looking spelling was the broken one, and it only
  showed up because two commands shared one redirect.

### Step 2 (original text) — promote the derived layer to `pcomm-read --link`

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

### Step 3 — **DONE 2026-08-09** — `pcomm-ui`, the matrix panel

`pcomm-ui <export-dir> [--port N] [--dir <static-dir>]`. DuckDB linked, page
embedded, bound to loopback. Verified against `scale_nccl-44445029`: a correct
32-rank ring — bright diagonal (`r→r+1`, 1.3 GB), sub-diagonal (`r→r-1`, 158 KB
of acks), wraparound corners, 64 edges, 39 GB total — with the numbers matching
`--sql` run by hand.

32 ranks, not the 64 the acceptance names, because `acc_debug` caps at 8 nodes.

**One data path, enforced by having only one.** Everything the page knows
arrives through `POST /query`; there is no endpoint that computes anything
server-side, and adding one is how the derived layer forks and two panels start
disagreeing about total bytes. It is also what keeps the DuckDB-WASM mode
reachable — swapping the transport under one function is a different job from
hunting fetches through a UI.

**Not Svelte yet, deliberately.** The page is hand-written HTML/SVG with no
build step, because step 3's job is to prove the spine
(Parquet → DuckDB → `/query` → marks) before eight panels ride on it. The
`--dir` seam already serves a built frontend, so the Svelte app replaces the
built-in page without the server learning anything about it. **Before that
lands: `node_modules` is not in `.gitignore` and `~/pockt` is syncthing-mirrored
— a frontend install would replicate to every host.** Ignore it in both places
first.

Design notes that came from the corpus rather than from taste:

- **Log scale, because step 0 measured five orders of magnitude** in a real
  matrix. Linear renders that as two lit cells on a dead grid.
- **The sequential ramp is defined against its surface, and reversing it for
  dark mode is not optional.** The first render mapped the largest values to the
  darkest step, so on a dark background the 1.3 GB diagonal read *quieter* than
  the 158 KB sub-diagonal beside it. Caught only by rendering the page and
  looking at it; `?theme=light|dark` now exists so both modes stay checkable.
- **`dst_rank = -1` gets its own column, marked in the critical colour**, never
  dropped. A matrix that silently omits unattributed traffic lies about its
  totals, and this is the visible form of the risk this plan carries.
- `link_health` is surfaced as a banner when non-zero.
- Per-cell hover tooltip, a legend, and a table view behind a disclosure, so
  identity is never colour-alone.

**One honest limitation in the server.** The `/query` guard is a first-token
check, and it is the only one: an in-memory DuckDB cannot be opened READ_ONLY,
and DuckDB's table functions mean a `SELECT` can read any file the user can. At
loopback, serving one local capture to the person who started it, that is the
same trust boundary as their shell. It would not be if this ever grew a bind
address — that is the moment for `SET enable_external_access=false` and a real
parser, and the code says so at the call site.

### Step 3 (original text) — `pcomm ui`, one panel

Server + embedded Svelte app + the **communication matrix only**. One panel, end
to end, over a real corpus capture.

The point is to prove the whole spine — Parquet → DuckDB → `query()` → Vega —
before eight panels are riding on it. Second panel only after the first is
genuinely good.

Acceptance: `pcomm ui <corpus/nccl-64>` renders a correct 64×64 matrix, and the
numbers match a DuckDB query run by hand against `comm_edges.parquet`.

### Step 4 — **STARTED 2026-08-09** — the Svelte app

`ui/` is a Svelte 5 + Vite + Tailwind app, built to `ui/dist` and served by
`pcomm-ui --dir ui/dist`. Two panels so far: the matrix, ported from step 3's
hand-written page, and **per-rank traffic**. The built-in page stays as the
zero-build fallback, and the server did not change to accommodate any of this —
the `--dir` seam was already the whole interface.

Verified against `scale_nccl-44445029` in both themes. The rank panel is also an
independent check on the matrix: every rank sends 1.2 GB, receives 1.2 GB, and
has exactly **2 peers**, which is what a ring means.

The rules the app is built on, all in `ui/README.md`:

- **Every panel reads `src/lib/query.js` and nothing else imports `fetch`.**
  A panel with a private endpoint forks the derived layer; and the WASM build
  swaps one function instead of a UI's worth of fetches.
- **A panel that needs a table it does not have asks `--link` for it**, never
  computes it locally. That is what keeps eight views projections of one model.
- **Colour is tokens, Tailwind does layout.** The palette pairs must swap as a
  set, and `viz.js` reads the same steps for SVG fills.
- `viz.js` owns the ramp and the log scale, so panels cannot drift into private
  palettes — the thing that makes eight panels read as one tool.

Two a11y decisions worth keeping: the matrix uses **one delegated pointer
handler** rather than one per cell (a 64-rank matrix is ~4k marks), and its
marks are `aria-hidden` under a single labelled image, with the table view as
the accessible path to the same numbers — 4k focusable rects would be worse than
the table for everyone. The per-rank panel's tooltip was removed outright: every
value it showed is already a column.

#### All eight panels exist — 2026-08-09

Matrix, Ranks, Clustering, Flow, Objects, Source, Timeline, Ask. One renders at
a time, selected by URL fragment so a view is linkable. Each is a `<Panel>` with
four states — loading, failed, **empty**, data — because "this capture has no
such data" and "this panel is broken" must not look alike.

Three findings the panels produced, none of which was visible from the data:

- **Clustering falsified its own caption.** I wrote that a ring should pair
  neighbours; it splits even ranks from odd. That is correct: rank 0 sends to
  {1, N−1} and rank 1 to {0, 2}, so adjacent ranks share *no* peer and sit
  maximally far apart, while 0 and 2 both talk to 1. Cosine-over-peer-vectors
  measures "shares peers with", not "talks to" — a real property of the metric,
  and the caption now says so instead of predicting the opposite.
- **Objects shows device 65535 for 4,393 of 4,553 allocations** (only 40 per GPU
  resolve). That is the gap `PLAN-20260806-1329-generic-memory-attribution`
  tracks, now visible rather than latent.
- **`pcomm-ui` did not mount empty tables**, so any panel naming a stream the
  capture lacks failed outright — the Flow panel died on `mlx5_device_send` for
  every verbs-only capture. Fixed by sharing `--link`'s policy: `duck_mount.hpp`
  is now used by both, behind a `pcomm_duck` INTERFACE target that also carries
  the generated header. Two copies of that policy would have been one copy and
  one bug.

Also fixed: the generated `raw_columns.hpp` did not include the headers
declaring the record types — it compiled in `pcomm-read` only because main.cpp
happened to include them first. The generator now emits its own includes,
derived from where each record is declared.

Sankey confirmed the plan's prediction: there is no sankey transform in Vega, so
the layout is ~60 lines in `lib/sankey.js`. Ribbons stack by the position of the
node at the far end rather than by query order, which removed crossings that
were an artefact of `ORDER BY sends DESC`; the crossings that remain are real.

**Still to come in this step:** nothing new — but Flow shows raw opcode numbers,
because naming them belongs in a derived table in `--link`, not in a second copy
of `wr_opcode_name()` in the browser. That is step 2's remaining work, and so is
the `intern` table the Source panel needs to turn file handles into paths.

#### The object view, and the GPU view it turned out to need — 2026-08-10

Step 4's list named a "GPU view" and I nearly skipped it as vague. Reading
Snoopie (ICS'24) for what attaches to the object view showed it is the object
view's stated complement, and that the object view I had shipped was keyed on
the wrong column.

The paper gives each GPU its own object-view tab. I tabbed on **rank**. Those
are different columns and `gpu_access` carries both: `run_device` is the GPU
that ran the kernel, `mem_device` the GPU whose memory was touched. On the
4-GPU ring capture a single process drives all four devices, so rank collapsed
four GPUs into one list — a ring displayed as a self-loop. This is the same
error as the `(lid, qp_num)` keying: a process is not a device, and the capture
had said so all along.

So: **Objects** tabs on `mem_device` (the GPU as owner) and names, per object,
the GPU that reached in. **Devices** tabs on `run_device` (the GPU as actor)
and carries the paper's two tables — objects touched, each marked local or
remote, and the instruction-type mix. Being the same rows read from opposite
ends makes them cross-check each other: on the ring, Objects/GPU 0 reports
"touched by GPU 3" and Devices/GPU 3 reports "memory on GPU 0, remote", 16,384
stores both ways.

Two of the paper's features are deliberately absent. **Source lines** appear in
its object hover and device table; out of scope. The **2D reshape** needs
dimensions the capture does not record and the user would have to supply — the
1D heatmap is what the data supports.

The object view is also the first view that reads the core object registry: on
IBGDA captures with no gpumem it lists the application's 4 KB buffers at the
*same address on both ranks*, which is the symmetric heap, visible without
NVBit for the first time.

**Superseded — the original list of remaining panels:** dendrogram, sankey,
object view, source view.
The first two are where Vega's grammar may not reach — `vega-hierarchy`'s
`tree`/`cluster` should cover the dendrogram, but there is no sankey transform,
and the dendrogram↔matrix ordering lock is where a spec stops being small. Both
source trees are vendored under `reference/`; read them before concluding a
figure is out of reach.

### Step 4 (original text) — the remaining curated panels

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
- ~~**`comm_edges` accuracy at multinode scale.**~~ **Retired 2026-08-09** by
  the 32-rank capture: exact across 8 nodes, zero unresolved, zero self-edges.
  The risk as written was inherited from the `qp_num`-only key. It returns as an
  open question above 32 ranks, which nothing has measured — so the UI should
  still surface `dst_rank = -1` as a visible count rather than dropping those
  rows, because that is the signal if it ever does break.
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
