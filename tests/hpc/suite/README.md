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
| 05 | `nccl_p2p_xfail` | 1 node × 4 H100      | NVLink P2P         | `p2p` | **xfail (deadlock)** |

Cases 01–04 passed the determinism gate (two independent MN5 runs → byte-identical).

**Case 05 is a known-failing (xfail) test, kept on purpose.** Tracing NCCL over
NVLink with the gpumem provider is a major use case that currently deadlocks (see
below). We do **not** delete or weaken the test to go green — it stays as an
`expect_fail` case (`meta.json`), bounded by a `timeout`, and `run_suite check`
reports it **XFAIL** (expected — suite stays green) or **XPASS** (an alert: it
started working, so remove the marker and add goldens). Its `checks/*.sql`
document the capture we'll assert once it's fixed.
Cases 02 (inter-node IB) and 03 (intra-node NVLink) are the "who moves data to
whom" question on opposite transports/providers: case 02's `comm_edges` vs case
03's `peer_topology`.

**Why case 03 is a custom kernel, not NCCL:** NCCL runs a *persistent* collective
kernel that occupies the GPU, so NVBit's bounded channel deadlocks (the gpumem
provider inits, NCCL prints its version, then hangs). That's intrinsic to
persistent-kernel collectives under NVBit — Snoopie has the same limit. A
discrete peer-write microkernel is the right NVBit target and a cleaner oracle;
NCCL's inter-node story is captured on its IB side by case 02.

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
