# NOTES — round 2, offline. Traps that cost time with no allocation involved.

Round 1's `NOTES.md` still holds in full and is not repeated here: the module
load order, `mpirun --bind-to none` instead of `srun`, `comshu_load_runtime`
being the wrong runtime for QE, `-mca coll_hcoll_enable 0`, and the teardown
SIGSEGV. Read it first. This file is only what round 2 added.

**Nothing here has been run.** MN5 was unreachable for the whole session:
alogin1 and alogin2 silent on 22, 443 **and** 80, while www.bsc.es:443 and
github.com:22 answered from the same machine — so not a port block and not our
network. Everything below came from source, schema and a synthetic database.

## The knob table, settled from QE's source before choosing one

Worth writing down because it is not in QE's own documentation in this form, and
because three of the four advertised levels are unavailable on a GPU build:

| flag | on `quantumespresso/7.5-nvidia-nvhpcx` | citation |
|---|---|---|
| `-ntg` / `-nyfft` > 1 | **aborts.** `errore('mp_startup','No task groups for GPUs')` | `Modules/mp_global.f90:83-84`, guarded by `#if defined(__CUDA)`, and `install/m4/x_ac_qe_cuda.m4:91` defines `-D__CUDA` unconditionally for any GPU build |
| `-nband` | works. Forced to 1 only under `__CUDA_OPTIMIZED`, which **neither** build system defines | `Modules/mp_global.f90:107-111` |
| `-ndiag` | works, but partitions the ScaLAPACK ortho grid — a **different** communicator whose default size is a function of `nproc_bgrp`, i.e. of the thing `-npool` changes. Pin it in both arms or the diff is not attributable | `LAXlib/mp_diag.f90:20-34` |
| `-npool` | works, and is the only one whose partition can be made to align with the node boundary | `Modules/mp_pools.f90:73-92` |

## The topology is the experiment, not a detail

Round 1 ran 4 nodes × 1 rank. That makes every pool boundary a node boundary, so
`-npool` moves the rank matrix and cannot move the transport. Round 2 runs
2 nodes × 4 ranks with `--map-by ppr:4:node`, and then

    a pool spans a node boundary  <=>  npool < nodes

because QE's pools are contiguous blocks (`mp_pools.f90:79`) and block placement
makes `node(r) = r / (ranks/nodes)`. **If you change `--map-by` you invert the
whole experiment and nothing errors.** It is pinned in `job-r2.sbatch` with that
warning beside it.

## Write assertions that survive both SQL engines

`sql_check.sh` prefers Parquet (DuckDB) and falls back to CSV (sqlite3), and
this case's own `check-r2.sh` uses sqlite3. Integer division is **not** the same
on the two: sqlite `8/3` is 2, DuckDB `8/3` is 2.666…. So an oracle written as
`ranks/npool` changes value with the engine, which makes it not an oracle.

Two fixes, both used here:

* express predicates as multiplications or as comparisons that cancel the
  division — `npool < nodes` rather than `ranks/npool > ranks/nodes`; `2 * bytes
  * (nproc_bgrp - 1) >= total` rather than a share against `0.5/(P-1)`;
* declare the *results* of any division as params in `meta-r2.json`
  (`nproc_bgrp`, `fft_nr3px`, `fft_ncpx`) with the formula in the comment, so
  the SQL only ever multiplies.

## Unit-test the assertions against a synthetic database, before the slot

Costs about ten minutes and it is the single highest-value offline step. Build
one sqlite database per arm with the right column names, populate a healthy
capture, confirm every assertion passes; then inject each defect one at a time
and confirm the corresponding assertion **fires**. For this round: 10 assertions
green on both arms, 18 injected defects all fired, and the two cases designed to
stay silent (a `link_health` row with `n = 0`; a capture with no tag doors at
all) stayed silent.

It paid for itself three times before any allocation existed:

* the `npool = 2` arm of the headline assertion passed on an **empty**
  `transport_mix` — the exact "a capture of a workload that did not work is
  indistinguishable from a tracer that captured nothing" hole `apps/README.md`
  warns about. Two more clauses now guard it (`all_ops = 0`, and `local_ops = 0`
  where four ranks share a node);
* the hex branch of the byte oracle had never been executed, because the healthy
  fixture rendered `count` in decimal. Feeding it `0xd0200` is what proved the
  branch works rather than merely existing;
* `verbs_doors_stay_blind-r2.sql` used a bare `HAVING count(*) > 0` with no
  `GROUP BY`. sqlite3 accepts it, but an aggregate with no grouping is not
  portable, so it was rewritten as `WHERE EXISTS`.

The column names to build the fixture from are **not** all in one place — see
`QUESTIONS.md` items 1 and 12. `transport_mix` is
`rank, scope, tl_name, op, ops, out_bytes, in_bytes, advertised_bytes`,
recovered from `21_ucx_deep`'s assertions and job script.

## Do not answer the managed-memory question at runtime

Round 1's warning, restated because it is the one that produces a *confidently
wrong* answer rather than no answer: an `LD_PRELOAD` interposer on
`cudaMalloc`/`cudaMallocManaged`/`cuMemAlloc_v2` measures **zero for every
counter** on this stack, including plain `cudaMalloc`, on a run with seconds of
GPU time. NVHPC's OpenACC runtime does not reach the CUDA allocators through the
dynamic symbol table, and CUDA 12 hands out driver entry points via
`cuGetProcAddress`.

Round 2 adds a cheaper and stronger line than `ldd`: **read QE's build system.**
`install/m4/x_ac_qe_cuda.m4:97,119,120,123-125` is the complete GPU flag set —
`-cuda -gpu=cc<CC>,cuda<VER>` plus `-acc` — and there is no `-gpu=managed` and no
`-gpu=unified` anywhere in it; `CMakeLists.txt:61-68` offers `QE_ENABLE_CUDA` and
`QE_ENABLE_OPENACC` and no managed option exists to set. Those two flags are the
only ones that link `libnvhpcman.so`. That settles it for **any** QE 7.5 build,
not just the one binary on this machine, which `ldd` cannot do.

## Fetching QE's source needs no cluster

`raw.githubusercontent.com/QEF/q-e/qe-7.5/<path>` and the tree listing at
`api.github.com/repos/QEF/q-e/git/trees/qe-7.5?recursive=1`. Nine files were
enough for every claim in `PREDICTION.md`. MN5 has no outbound network, so this
is login-node-free work that belongs on the local machine anyway.
