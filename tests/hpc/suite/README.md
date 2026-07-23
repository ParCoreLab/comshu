# pcomm integration suite

Regression tests that answer one question concretely: **does the tool still
capture what a real workload produces?** Each case runs a genuine communication
workload on MareNostrum 5 under `pcomm`, then `check_capture.py` asserts the
expected record streams are present, non-empty, and cover the expected ranks.
The assertion is provider-agnostic — it checks the `<rank>/<type_id>.pcs` streams
the tracer writes — so it catches the regressions that matter (a provider went
silent, cross-thread flush dropped proxy records, replay passes collided) without
needing a decoder per record type.

## The cases

The axis that matters for a comm profiler is **which transport / provider**:

| # | case | topology | transport | provider | what it guards |
|---|------|----------|-----------|----------|----------------|
| 01 | `ib_pingpong`    | 2 nodes, 1 rank each | IB verbs, direct | `ib`  | baseline: hook engine → records, both endpoints |
| 02 | `nccl_internode` | 2 nodes × 4 H100     | IB (P2P disabled) | `ib`  | at-scale IB, GPUDirect MRs, proxy-thread flush |
| 03 | `nccl_p2p`       | 1 node × 4 H100      | NVLink P2P        | `p2p` | GPU-initiated intra-node peer r/w (NVBit) |
| 04 | `nvshmem`        | 1 node × 4 H100      | NVLink, sym. heap | `p2p` | PGAS put/get, symmetric-heap MemObjects |

02 and 03 are the same NCCL all-reduce with opposite transport (IB vs NVLink) and
opposite provider — the "inter-node, no p2p" and "intra-node p2p" pair.

## Running

```bash
# from the repo root on MN5 (~/proj/pcomm)
tests/hpc/suite/run_suite.sh list
tests/hpc/suite/run_suite.sh submit                 # all cases (sbatch each)
tests/hpc/suite/run_suite.sh submit 01_ib_pingpong  # one case
tests/hpc/suite/run_suite.sh status                 # squeue
tests/hpc/suite/run_suite.sh check                  # aggregate PASS/FAIL once jobs finish
```

Each job also runs the checker itself and prints the verdict in its `.out` file;
`check` re-aggregates the `result.json` each job drops in its output dir
(`$HOME/pcomm-suite/<case>-<jobid>/`).

## Prerequisites

- **Build** `pcomm`, `pcomm-read`, and the providers under `~/proj/pcomm/build`.
  The `ib` cases need `-DPCOMM_PROVIDER_IBVERBS=ON` (builds on MN5 today). The
  `p2p` cases (03/04) need `-DPCOMM_PROVIDER_GPUMEM=ON`, which needs **NVBit** —
  not yet staged on MN5. Stage it (e.g. NVBit 1.7.7 from du04
  `~/waste/nvbit-Linux-x86_64-1.7.7.1.tar.bz2`), point the gpumem CMake at it,
  and rebuild. Until then 03/04 are ready-to-run but unvalidated.
- Toolchain split (see `common.sh`): compile the CUDA apps with the gcc-11.4
  host (nvcc 12.9 rejects gcc-13.2's `__is_convertible`), run with gcc-13.2
  libstdc++ available (the providers need `GLIBCXX_3.4.31`).

## Known gaps (see ../../../things-to-circle-back-to.md)

- **Replay under MPI.** Every case here uses a *single* provider, so `pcomm`
  exec's directly — no replay. A mixed selection (`-p ib,p2p`) makes the
  launcher fork a pass per exclusive provider *inside each MPI task*, which
  breaks PMI wireup (the task stops being the MPI rank; a second `MPI_Init`
  after the first pass's `MPI_Finalize` is unsupported). For MPI jobs, replay
  must wrap `mpirun` (run pass 1 for all ranks, then pass 2), not fork per rank.
  A combined intra+inter case is deferred until that lands.
- The `p2p` cases assert capture happened, not owner attribution — NVSHMEM 3.7's
  fragmented heap breaks the positional ownership heuristic.
