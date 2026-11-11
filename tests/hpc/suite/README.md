# comshu integration suite

Regression + correctness tests that answer one question concretely: **does the
tool still capture the *right* thing for a real workload?** Each case runs a
genuine communication workload on MareNostrum 5 under `comshu`, exports the
capture with `comshu export --csv`, and runs SQL over it.

## Two kinds of check, and why

Each case carries both, and they answer different questions.

| | `asserts/<name>.sql` | `checks/<name>.sql` + `.golden` |
|---|---|---|
| shape | returns the **offending rows**; empty = pass | output **diffed** against a committed table |
| oracle | the workload's own parameters, from `meta.json`'s `workload` block | a previous run |
| answers | "is this capture correct?" | "did this capture change?" |
| on failure | prints what violated the claim | prints a diff of two blobs |

**Assertions are primary. Goldens are the regression net.** A golden is a
characterization test, and it fails two ways:

- *It cannot catch a bug that predates it.* `04_nvshmem`'s `peer_fanout` golden
  asserted `peers = 2` for a ring in which each PE puts to one neighbour — its
  own comment says so. It had captured the NVSHMEM device-attribution bug and
  made it the expected result. No golden can ever find that, because the golden
  **is** the defect. The derived assertion beside it fails on day one, which is
  the point.
- *It fails on things that were never the claim.* The fabric handed both ranks
  of `01_ib_pingpong` `qp_num 449` (job 44111484) and again `461` (44113361);
  `comm_edges` maps sends to ranks through `qp_num`, so rank 0's edge collapsed
  onto itself and the topology golden went red twice with nothing wrong in the
  tool. The assertion — fan-out 1 carrying `iters` sends of `msg_bytes` — passes
  on both.

The rule that keeps an assertion honest: **it must either reference `params`, or
be an invariant that names no specific rank, device or QP number.** If it names
an identifier the run produced, it is a golden wearing a costume.

`params` is a one-row table `sql_check.sh` materialises from `meta.json`:

```json
"workload": { "ranks": 2, "nodes": 2, "iters": 20, "msg_bytes": 4096,
              "regions_per_rank": 1 }
```

```sql
-- asserts/traffic.sql — each rank posts exactly iters sends of msg_bytes
SELECT rank, count(*) AS sends, sum(CAST(length AS INTEGER)) AS bytes
FROM ib_post_send GROUP BY rank
HAVING sends != (SELECT iters FROM params)
    OR bytes != (SELECT iters * msg_bytes FROM params);
```

The numbers were always in `job.sbatch`; the `workload` block is the first place
they are written down as an *oracle* rather than as shell variables.

Supporting machinery, all in `meta.json`:

- `expect_tables` — every one must exist and be non-empty. Not written as SQL,
  because the failure it guards against is SQL not running at all: this suite
  has twice reported STABLE on a `no such table` error. It is what found
  `08_ib_send_bw`'s empty `ib_queue_pair`.
- `expect_fail_asserts` — `{"<assert name>": "<why, with the blocking plan>"}`.
  Reports `[xfail]` and does not fail the case; if it starts passing it reports
  `[XPASS]` loudly. **Never edit an assertion to match what we emit** — mark it,
  with evidence, and let the fix flip it.
- `-- requires: <table> params.<col>` at the top of an assertion. Reports
  `[skip]` when the capture has no such table or the workload no such
  parameter, so `suite/asserts/*.sql` can be genuinely cross-cutting.

And one table that is *not* from `meta.json`:

- `provenance(expect_commit)` — what the RUNNER says this board is about,
  materialised from `$COMSHU_EXPECT_COMMIT`. `params` is the workload's claim;
  this is the run's. It has **zero rows when no runner named a commit**, so
  `-- requires: provenance` makes a hand-run report `[skip]` rather than a
  vacuous `[ok]` — see the vacuous-pass section below, which applies with
  unusual force here: a capture of unknown provenance is exactly the state the
  check exists to refuse. `run_on_mn5.sh` and `run_on_a100.sh` both set it, and
  every board prints whether it is armed.

`suite/asserts/` holds the checks worth running everywhere: `owner_in_range`
(every `mem_dev` names a GPU that exists), `completions_succeed`,
`sends_have_payload`, `ranks_complete`.

### The two assertions about the tool rather than the workload

Four false findings have come from a capture produced by something other than
the tree under test — a stale `.so` that invented a planner bug, a hand-copied
provider that nearly refuted a correct change, a syncthing-mixed tree, a gate
green over uncompiled code. Each assertion's header names them. They divide the
question three ways and none of them subsumes another:

| | asks |
|---|---|
| `build_is_the_boards_commit` | is what ran the commit this board claims? (`invocation.build` vs `$COMSHU_EXPECT_COMMIT`) |
| `images_are_no_older_than_the_engine` | were the objects that actually loaded built together? (`invocation.images` mtimes, 60 s threshold measured off job 45426015) |
| `suite/check_build_freshness.sh` | did you build at all since editing a compiled file? — preventive, run by both drivers before they submit |

The build id is compiled into the *engine*, so a stale provider linked against
it reports the engine's commit quite happily: only the image mtimes see that.
A stale engine under fresh providers is the reverse, and only the commit sees
that. The freshness script is the only one of the three that answers before the
queue wait rather than after it.

## How correctness is verified

The chain is: **deterministic workload → capture → `comshu export --csv` → SQL → assert / diff golden.**

- **Tests bind to the CSV column contract, not the on-disk format.** A check is
  a `.sql` file querying columns like `ib_post_send(rank, opcode, length)` or the
  derived `comm_edges(src_rank, dst_rank, sends, bytes)`. If the `.pcs` binary
  format changes, the reader changes; the tests don't. Improving the tools
  (new columns, new derived tables) is how we make new properties checkable —
  e.g. `comm_edges` exists so topology is asserted in rank-space, never against
  run-specific QP numbers or LIDs.
- **Byte-repeatable by construction.** Each query aggregates away everything that
  varies run-to-run (timestamps, driver-assigned qp_num/LID *values*, addresses,
  wr_ids) and `ORDER BY`s its output, so a golden is stable. Workloads are pinned
  (`ibv_rc_pingpong -n`; NCCL `Ring/Simple`, fixed channels) so counts reproduce.
- **The golden encodes *known* values, not just whatever ran.** e.g. case 01's
  golden says `send, 20, 81920` because `pingpong -n 20` provably posts 20 sends
  of 4096 B — an independent oracle. The diff then catches regressions.
- **Determinism is gated, not assumed.** A golden is only committed after the
  same query yields byte-identical output across two independent runs. Case 01
  passed this gate (two separate SLURM jobs → identical tables).

Engine is **sqlite3** (present on MN5 and locally, zero setup). Queries are kept
portable ANSI SQL, so DuckDB drops in via `SQL_ENGINE` if ever wanted.

## The cases

Axis = **which transport / provider**:

| # | case | topology | transport | provider | golden |
|---|------|----------|-----------|----------|--------|
| 01 | `ib_pingpong`    | 2 nodes, 1 rank each | IB verbs, direct   | `ib`  | **verified** |
| 02 | `nccl_internode` | 2 nodes × 4 H100     | IB (P2P disabled)  | `ib`  | **verified** |
| 03 | `p2p_kernel`     | 1 node × 4 H100      | NVLink peer r/w    | `p2p` | **verified** |
| 04 | `nvshmem`        | 1 node × 4 H100      | NVLink, sym. heap  | `p2p` | **verified** |
| 05 | `nccl_p2p` | 1 node × 4 H100      | NVLink P2P         | `p2p` | pass (~435s) |
| 06 | `nvshmem_coexist` | 1 node × 4 H100     | NVLink, sym. heap  | `ib`+`p2p` | **verified** |
| 07 | `nvshmem_multinode` | 2 nodes × 4 H100 | NVLink + IB   | `ib`+`p2p` | pass |
| 08 | `ib_send_bw`     | 2 nodes, 1 rank each | IB verbs, **newer entry points** | `ib`  | **verified** |

Cases 01–04 and 06 passed the determinism gate (two independent MN5 runs →
byte-identical query output; the raw CSVs always differ, they carry ticks,
addresses, `qp_num` and `wr_id`).

**Case 06 is the coexistence regression test.** It is case 04's workload and
topology with exactly one variable changed — both providers loaded into one
process — so a coexistence regression shows up here and nowhere else, every
other case loading a single provider. Its `traffic` golden is *identical* to
case 04's, which is the actual assurance: adding `ib` does not perturb what
`p2p` captures. Scope, stated honestly: on one node every peer is
NVLink-reachable, so NVSHMEM builds its IB transport but never posts a send —
the `ib` side contributes entities (MR/QP/modify_qp), not traffic, and
`ib_post_send`/`ib_poll_cq` are legitimately empty.

Case 08 exists because case 01 could not catch a whole class of blindness.
`ibv_rc_pingpong` posts through the classic `ibv_post_send`, so it passes
whether or not the newer entry points are captured — and for a long time they
were not, silently. perftest produced 202,000 completions and zero sends
(`ibv_wr_*` never touches `ctx->ops.post_send`), and no memory regions at all
(`ibv_reg_mr` is a macro that falls through to `ibv_reg_mr_iova2` whenever the
access flags are not a compile-time constant). The two cases together distinguish "sends are
captured" from "sends are captured for one API". Its goldens come from the
`-n`/`-s` arguments rather than from a previous run, so the case cannot ratify
whatever we happen to emit.

**Case 07 is the unified NVLink+IB *traffic* capture, and it passes.** Eight PEs
over two nodes, both providers in one process, one execution: six ring hops go
over NVLink and two cross the node boundary over IB.

    rank  nvlink_accesses  ib_sends
    0..2  196608           ~85          <- NVLink hops
    3     0                196691       <- PE3->PE4 crosses nodes
    4..6  196608           ~82
    7     0                196690       <- PE7->PE0 crosses nodes

The asymmetry is the point, not a defect. Inter-node NVSHMEM does not move
payload with GPU loads/stores into a PGAS window — the device enqueues a
descriptor and the NIC performs the transfer — so an address tracer sees
*nothing* on those hops (`nvlink_accesses = 0`), while `ibverbs` sees all of it.
Pairing the two providers is what makes the whole ring observable, and doing it
in one execution is what makes cross-transport causality recoverable; replay
cannot express this, because its passes are different runs.

It was xfail from 2026-07-21 to 2026-07-28. The cause was not coexistence, not
the bounded channel and not multinode as such: the backend instrumented
LOCAL-memory accesses (per-thread stack, spilled registers) inside NVSHMEM's
inlined proxy-enqueue path, which wedged the kernel. Bisected to a single
`LDL.64` in `nvshmemi_transfer_rma_p<int>` and fixed by instrumenting only
GLOBAL/GENERIC/GLOBAL_TO_SHARED (`3c5d55f`). `NVSHMEM_DISABLE_P2P=1` reproduces
the old hang on a single node — that is what isolated it. The mechanism is still
unexplained; see things-to-circle-back-to.md.

**Case 05 runs NCCL over NVLink and passes, but takes ~435s.** That is a fixed
cost of NVBit processing NCCL's module — 223 device-function specializations,
434k instructions — not of the workload. Measured at N=100, 1024, 16384, 65536
and 262144: every one takes 433-443s while the capture scales from 0.6 MB to
47 MB; uninstrumented is 6s. Restricting instrumentation to the single launched
kernel does not help (438s vs 436s), and neither does skipping the per-launch
arming of the other 222. Its DEADLINE is 600s for that reason. Do not try to
make it faster by shrinking the message — that was tested across a 2621x range
and changes nothing.

Its checks assert structure rather than volume, because volume is not
repeatable: NCCL's LL protocol spins on flag polling, so two identical runs
recorded 295,044 and 550,064 loads on rank 0. See the case's meta.json.

**Why case 04 filters kernels:** unfiltered, NVBit instruments all of nvshmem's
internal device code (~400 MB/rank, not repeatable). `COMSHU_KERNEL_NAME=put_to_next`
restricts capture to the user kernel → a small deterministic golden.

**Case 04 is also where the goldens were caught ratifying a bug.** NVSHMEM 3.7's
fragmented heap breaks positional device attribution, and the `peer_fanout`
golden had turned that into the expected result: `peers = 2` for a ring in which
each PE puts to exactly one neighbour. Its three `asserts/` state the workload
instead, and two of them are marked `expect_fail` against
`PLAN-20260803-1448-nvshmem-heap-attribution`:

- `traffic` — passes. 196608 peer stores of 4 B per rank. We capture the right
  accesses and the right number of them.
- `peer_fanout` — one peer per device. Reads 2, because one real edge is split
  across two adjacent phantom devices (`mem_dev` 65 and 66 in job 44113361, 238
  and 239 in 44111484).
- `no_local_loads` — `put_to_next`'s only peer access is the store; `src[i]` is
  a local read and `gpu_access` is peer-only by construction. Reads 196608
  phantom loads per rank, which is the doubled access count that went
  unexplained for three weeks. `07_nvshmem_multinode` runs the identical binary
  with correct attribution and records zero loads — that is the proof the claim
  is right, which is why the same file is unmarked there.
- `owner_in_range` (cross-cutting) — `mem_dev < gpus`. Reads 65..69 / 238..242
  on a four-GPU node. The most direct evidence of the fault, and it costs one
  scan on every case that has GPU accesses.

## Layout

```
cases/<case>/
  job.sbatch          runs the workload under comshu, exports CSV, runs sql_check
  meta.json           providers, topology, and the `workload` oracle + expectations
  asserts/<name>.sql      derived check: offending rows, empty = pass
  checks/<name>.sql       one deterministic query each
  checks/<name>.golden    its expected result table
suite/
  asserts/<name>.sql  cross-cutting assertions, run for every case that has the tables
  sql_check.sh        loads the CSVs into sqlite, runs asserts + diffs goldens
  run_suite.sh        {list, submit, check, status}
  common.sh           MN5 env (accounts, modules, provider paths)
```

## Adding a case

**A new case needs at least one derived assertion in `asserts/`. A golden alone
is not a test of correctness** — it can only ratify whatever the tool happened
to emit the first time, which is how `04_nvshmem` shipped a green check over a
live attribution bug. Concretely:

1. Write `meta.json` with a `workload` block (the numbers from your `job.sbatch`,
   nothing read off a capture) and `expect_tables`.
2. Write `asserts/*.sql` that derive the case's volume and shape from `params`.
   If you cannot derive anything, say why in `workload_note` — `02` and `05`
   both do, and both still assert symmetry and participation, which are
   invariants that need no oracle.
3. Only then commit goldens, once the query is byte-identical across two
   independent runs.

Check yourself against the one rule: an assertion that names a rank, a device
ordinal or a QP number, and does not reference `params`, is a golden in
disguise. Delete it and write the claim you actually meant.

### The vacuous pass, and why it deserves its own heading

**An assertion whose convention is "empty result means pass" passes trivially
when its input is empty.** That is the single most common defect in this
suite's assertions, and it is invisible in a green board — a check that never
ran and a check that ran clean look identical.

It is not a hypothetical. On 2026-08-29 three independent authors each shipped
one, and each found it only by mutation-testing — not by review:

| where | the shape |
|---|---|
| `03_quantumespresso` | the `npool=2` arm filtered to rows a `npool=1` capture never contains |
| OSU study | `sum()` over no rows returns `NULL`, and `NULL > threshold` is not true |
| `02_gromacs` | hex-decode canaries over a table that was empty for an unrelated reason |

Its sibling is **passing for the wrong reason**, where the input is non-empty
but the rows satisfying the check are not the rows the case is about.
`22_ucx_rma` shipped one: "`ucx_atomic` is non-empty" went green on job
45177421 over 80 rows that were all `mca_osc_ucx`'s own window synchronisation,
while all 20 of the workload's own atomics failed with status -5. The fix was to
name a row only the workload could have produced — a *succeeding* `add`, since
MPI's internal locking uses `cswap` and `swap` and never `add`.

Note which way that case's assertions run: `NOT EXISTS ... WHERE (SELECT
rma_iters FROM params) > 0`. An empty table makes them **fail**, which is the
shape to copy — the `params` guard states when the claim applies, and the
`NOT EXISTS` states what must be there when it does. So:

1. **Mutation-test every assertion.** Break the exact thing it names — edit the
   fixture, flip a value, drop a row — and confirm it turns red. An assertion
   never observed failing is an assertion with no evidence behind it.
2. **Then delete its input and run it again.** If it still passes, it is
   vacuous. Add a companion that asserts the input is non-empty, e.g.
   `SELECT 'no atomics captured' WHERE (SELECT count(*) FROM ucx_atomic) = 0`.
3. **Watch the aggregates.** `sum`/`avg`/`max` over zero rows are `NULL`, and
   every comparison against `NULL` is false — so a threshold check on an empty
   table is a guaranteed pass. `coalesce(...)` first, or count separately.

The rule in one line: **a passing assertion must be able to name the rows it
looked at.** Empty-means-pass is a fine convention only when something else
guarantees the input was not empty.

## Running

**One command, from a checkout** (syncs to MN5, builds tools + both providers,
runs every case, prints the board):

```bash
tests/hpc/run_on_mn5.sh                 # sync + build + run all + check
tests/hpc/run_on_mn5.sh --cases "03"    # a subset
tests/hpc/run_on_mn5.sh --quick         # skip case 05 (see below)
tests/hpc/run_on_mn5.sh --per-case      # one job per case (isolating a case)
tests/hpc/run_on_mn5.sh --check-only    # just re-aggregate existing results
tests/hpc/run_on_mn5.sh --no-build      # skip the rebuild
```

Config via env (defaults = the ParCore MN5 account): `COMSHU_REMOTE`,
`COMSHU_REMOTE_DIR`. Exit code is the board's: 0 when all PASS/XFAIL, non-zero on
any real FAIL. It builds the gpumem provider with the gcc-11.4 host + CUDA C++20
recipe automatically.

**All cases run inside one allocation** (`suite/all_in_one.sbatch`). `acc_debug`
permits one *submitted* job at a time, so a job per case meant a queue wait per
case — that scheduling overhead, not the workloads, was most of the old
~25-30 min. The allocation is the union of what the cases ask for (2 nodes x
4 tasks x 4 GPUs) and each case's `job.sbatch` runs unmodified inside it: the
`#SBATCH` lines are comments, so it is the same program with the same
srun/mpirun shapes as a standalone `sbatch`. Nothing is forked into a suite-only
variant that could drift.

A combined job can die partway, which would leave later cases unreported — the
exact shape of the stale-PASS bug the manifest exists for. Every case is pinned
to the allocation's job id, so a case the job never reached has no
`<label>-<jobid>.csv` and reads as **no result** (red), never as an inherited
pass. `--quick` names the skipped case in the manifest, so it prints `SKIP` and
the board says INCOMPLETE rather than leaving a silent gap.

**Case 05 costs ~440 s that is not suite time.** It is NVBit lifting NCCL's
module — one-time preparation of the binary before any of our work starts, the
same kind of cost as a compile, charged every run only because it cannot be
cached. Measured invariant from N=100 to N=262144; see the case's `meta.json`.
The other seven cases together are ~150 s, so a full run is ~10 min of job time
and `--quick` is ~3. Do not read the 440 s as something the suite is doing.

The driver reports **queue wait and job time separately**. Queue wait is not
ours to improve, and folding the two together makes any change to the suite
itself unmeasurable. Per-case seconds land in `$COMSHU_OUT_ROOT/timings-<jid>.txt`
and each case's stdout in `$COMSHU_OUT_ROOT/<case>-<jid>.log`.

### Lower-level (on MN5 directly)

```bash
tests/hpc/suite/run_suite.sh list
tests/hpc/suite/run_suite.sh submit 01_ib_pingpong   # sbatch one (acc_debug = 1 job at a time)
tests/hpc/suite/run_suite.sh status
tests/hpc/suite/run_suite.sh check                   # aggregate PASS/FAIL

# regenerate a golden from a run you've independently verified:
comshu export <out> --csv <out>.csv
tests/hpc/suite/sql_check.sh <out>.csv cases/<case> --update
```

## Prerequisites / gaps (see ../../../things-to-circle-back-to.md)

- `comshu` and its providers built under `~/proj/comshu/build`. Build the
  `p2p` (gpumem/NVBit) provider on MN5 with the gcc-11.4 host and CUDA C++20:
  ```
  module load gcc/11.4.0 nvidia-hpc-sdk/25.7 cmake
  cmake -B build-gpumem -DCOMSHU_BUILD_TESTS=OFF -DCOMSHU_PROVIDER_IBVERBS=OFF \
        -DCOMSHU_PROVIDER_GPUMEM=ON -DCMAKE_CUDA_ARCHITECTURES=90 \
        -DCMAKE_CUDA_HOST_COMPILER=g++
  cmake --build build-gpumem --target gpumem_trace
  cp build-gpumem/providers/gpumem/libgpumem_trace.so build/providers/gpumem/
  ```
  (NVBit 1.7.7 lives at `gpu-backends/nvbit/nvbit-1.7.7.1/`; 1.8 needs CUDA 13.)
- Toolchain split (`common.sh`): compile CUDA with gcc-11.4, run with gcc-13.2
  libstdc++ (`comshu` needs `GLIBCXX_3.4.31` → `module load gcc/13.2.0`).
- **Replay under MPI** isn't per-rank fork (breaks PMI); every case uses a single
  provider so `comshu` exec's directly. A combined intra+inter case waits on
  mpirun-level replay.
- `comm_edges` maps QPs→ranks by qp_num, exact at 2 ranks / one HCA, approximate
  at multinode scale until the local LID is captured directly.
