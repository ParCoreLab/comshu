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
| 01 | `ib_pingpong`    | 2 nodes, 1 rank each | IB verbs, direct | `ib`  | **committed, verified** |
| 02 | `nccl_internode` | 2 nodes × 4 H100     | IB (P2P disabled) | `ib`  | pending pinned run |
| 03 | `nccl_p2p`       | 1 node × 4 H100      | NVLink P2P        | `p2p` | pending NVBit on MN5 |
| 04 | `nvshmem`        | 1 node × 4 H100      | NVLink, sym. heap | `p2p` | pending NVBit on MN5 |

02 and 03 are the same NCCL all-reduce with opposite transport (IB vs NVLink) and
opposite provider — the "inter-node, no p2p" and "intra-node p2p" pair.

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

- `pcomm`, `pcomm-read`, providers built under `~/proj/pcomm/build`. `ib` cases
  build on MN5 today. `p2p` cases (03/04) need **NVBit** (not yet staged on MN5;
  present on du04 `~/waste/nvbit-1.7.7.1`) → their goldens are pending.
- Toolchain split (`common.sh`): compile CUDA with gcc-11.4, run with gcc-13.2
  libstdc++ (`pcomm-read` needs `GLIBCXX_3.4.31` → `module load gcc/13.2.0`).
- **Replay under MPI** isn't per-rank fork (breaks PMI); every case uses a single
  provider so `pcomm` exec's directly. A combined intra+inter case waits on
  mpirun-level replay.
- `comm_edges` maps QPs→ranks by qp_num, exact at 2 ranks / one HCA, approximate
  at multinode scale until the local LID is captured directly.
