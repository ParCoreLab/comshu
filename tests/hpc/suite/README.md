# pcomm integration suite

Regression + correctness tests that answer one question concretely: **does the
tool still capture the *right* thing for a real workload?** Each case runs a
genuine communication workload on MareNostrum 5 under `pcomm`, exports the
capture with `pcomm-read --csv`, and runs SQL assertions whose results are
diffed against committed golden tables.

## How correctness is verified

The chain is: **deterministic workload → capture → `pcomm-read --csv` → SQL query → diff golden.**

- **Tests bind to the CSV column contract, not the on-disk format.** A check is
  a `.sql` file querying columns like `ib_post_send(rank, opcode, length)` or the
  derived `comm_edges(src_rank, dst_rank, sends, bytes)`. If the `.pcs` binary
  format changes, `pcomm-read` changes; the tests don't. Improving the tools
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
internal device code (~400 MB/rank, not repeatable). `PCOMM_KERNEL_NAME=put_to_next`
restricts capture to the user kernel → a small deterministic golden. NVSHMEM 3.7's
fragmented heap breaks positional device attribution, so `peer_fanout` asserts the
run-stable fan-out count, not the (unreliable) device ids; `traffic` is the real
volume oracle.

## Layout

```
cases/<case>/
  job.sbatch          runs the workload under pcomm, exports CSV, runs sql_check
  meta.json           providers + topology (informational)
  checks/<name>.sql       one deterministic assertion each
  checks/<name>.golden    its expected result table
suite/
  sql_check.sh        loads the CSVs into sqlite, runs each .sql, diffs golden
  run_suite.sh        {list, submit, check, status}
  common.sh           MN5 env (accounts, modules, provider paths)
```

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

Config via env (defaults = the ParCore MN5 account): `PCOMM_REMOTE`,
`PCOMM_REMOTE_DIR`. Exit code is the board's: 0 when all PASS/XFAIL, non-zero on
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
itself unmeasurable. Per-case seconds land in `$PCOMM_OUT_ROOT/timings-<jid>.txt`
and each case's stdout in `$PCOMM_OUT_ROOT/<case>-<jid>.log`.

### Lower-level (on MN5 directly)

```bash
tests/hpc/suite/run_suite.sh list
tests/hpc/suite/run_suite.sh submit 01_ib_pingpong   # sbatch one (acc_debug = 1 job at a time)
tests/hpc/suite/run_suite.sh status
tests/hpc/suite/run_suite.sh check                   # aggregate PASS/FAIL

# regenerate a golden from a run you've independently verified:
pcomm-read <out> --csv <out>.csv
tests/hpc/suite/sql_check.sh <out>.csv cases/<case> --update
```

## Prerequisites / gaps (see ../../../things-to-circle-back-to.md)

- `pcomm`, `pcomm-read`, providers built under `~/proj/pcomm/build`. Build the
  `p2p` (gpumem/NVBit) provider on MN5 with the gcc-11.4 host and CUDA C++20:
  ```
  module load gcc/11.4.0 nvidia-hpc-sdk/25.7 cmake
  cmake -B build-gpumem -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=OFF \
        -DPCOMM_PROVIDER_GPUMEM=ON -DCMAKE_CUDA_ARCHITECTURES=90 \
        -DCMAKE_CUDA_HOST_COMPILER=g++
  cmake --build build-gpumem --target gpumem_trace
  cp build-gpumem/providers/gpumem/libgpumem_trace.so build/providers/gpumem/
  ```
  (NVBit 1.7.7 lives at `gpu-backends/nvbit/nvbit-1.7.7.1/`; 1.8 needs CUDA 13.)
- Toolchain split (`common.sh`): compile CUDA with gcc-11.4, run with gcc-13.2
  libstdc++ (`pcomm-read` needs `GLIBCXX_3.4.31` → `module load gcc/13.2.0`).
- **Replay under MPI** isn't per-rank fork (breaks PMI); every case uses a single
  provider so `pcomm` exec's directly. A combined intra+inter case waits on
  mpirun-level replay.
- `comm_edges` maps QPs→ranks by qp_num, exact at 2 ranks / one HCA, approximate
  at multinode scale until the local LID is captured directly.
