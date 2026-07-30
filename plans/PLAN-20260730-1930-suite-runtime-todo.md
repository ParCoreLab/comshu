# PLAN — get a suite run under ~5 minutes

- **Created:** 2026-07-30 19:30 +03
- **Status:** todo
- **Now:** ~25-30 min wall clock for 7 cases.
- **Target:** ~5 min of job time once scheduled. Queue wait is outside our
  control, so measure and report the two separately — conflating them makes the
  improvement unmeasurable.

## Where the time actually goes

Measured run times inside the jobs (excluding queue wait):

| case | nodes | run |
|---|---|---|
| 01 ib_pingpong | 2 | ~10 s |
| 02 nccl_internode | 2 | ~30 s |
| 03 p2p_kernel | 1 | ~15 s |
| 04 nvshmem | 1 | ~20 s |
| **05 nccl_p2p** | 1 | **~440 s** |
| 06 nvshmem_coexist | 1 | ~16 s |
| 07 nvshmem_multinode | 2 | ~21 s |

Two separate problems, and they need different fixes:

1. **Six of the seven cases total under two minutes.** The wall clock is
   dominated by *per-case overhead* — `acc_debug` permits one submitted job at a
   time, so every case pays a fresh queue wait plus SLURM and MPI startup. Seven
   jobs, seven waits.
2. **Case 05 alone is ~440 s** and none of it is the workload. It is fixed NVBit
   module processing of NCCL's 223 device-function specialisations, confirmed
   invariant across N=100 to N=262144 (jobs 43971039, 43972064). Uninstrumented
   is 6 s.

## Step 1 — one allocation for the whole suite (biggest win, low risk)

Submit **one** job with 2 nodes x 4 GPUs and run every case inside it. All seven
fit that shape: 01/02/07 need two nodes, the rest need one. This removes six
queue waits and six job startups outright.

Keep the per-case `result.json` contract exactly as it is, so `run_suite.sh
check` and the manifest keep working unchanged.

*Expected:* ~9 min of job time (the sum above), from ~25-30 min wall clock.
*Acceptance:* same verdict board, one job id.

## Step 2 — run the independent cases concurrently

Inside that allocation, the four single-node cases (03, 04, 05, 06) can run on
separate nodes/GPU subsets at the same time, and 01/02/07 can overlap with them
as long as GPU and HCA assignments do not collide.

Watch out: cases 03 and 04 pin `PCOMM_RANK=0` for determinism, and captures land
under `PCOMM_OUT_ROOT` keyed by job id — concurrent cases must not share an
output directory. This is the same collision that made both `srun` steps write
to `rank-0` in the ibverbs benchmark.

*Expected:* total ≈ slowest case ≈ case 05 ≈ 440 s. Which means step 2 is
pointless without step 3.

## Step 3 — case 05's 440 s (the only thing that matters after step 1)

Everything else together is ~110 s, so the target is unreachable while case 05
is in the default path. Options, cheapest first:

- **Test NVBit lazy loading.** `NO_EAGER_LOAD=1` was tried against the *hang*
  and did not help, but was never measured against *cost*. If NVBit can avoid
  processing unused modules, this is a one-env-var fix. Try first.
- **Reduce what NVBit must process.** The cost tracks NCCL's 223 specialisations
  in one module, not our filter (438 s filtered vs 436 s unfiltered). Check
  whether a smaller NCCL build, or `NCCL_ALGO`/`NCCL_PROTO` at *build* level,
  yields fewer device functions. Unknown, possibly not available.
- **Tier it out of the default run.** Add `--quick` (default) that skips 05, and
  keep the full suite for nightly/pre-merge. This *hits the 5 min target
  immediately* and is the honest fallback if the first two fail — but it means
  the NCCL path stops being checked on every run, which is a real loss and
  should be a deliberate decision, not a silent default.

## Step 4 — trim the rest only if still needed

Case 02 (~30 s) is the next largest and is pinned to 200k iterations for
determinism. Reducing iterations changes its goldens, so it needs the two-run
gate again — only worth doing if steps 1-3 land and we are still over.

## Measure before and after

Record job time and wall clock separately, for a full run and a `--quick` run,
so the claim is checkable. The last few performance conclusions in this project
were wrong until measured, including two where I predicted the wrong direction.

## Watch out for

- **`acc_debug` is 2 h max.** One combined job must fit; with case 05 at ~7.5 min
  that is not close to binding.
- **Do not let a combined job hide failures.** Seven jobs fail independently; one
  job could abort partway and leave later cases unreported. The check step must
  distinguish "did not run" from "passed" — the stale-PASS bug (`fc8ccd6`) was
  exactly this, and the manifest exists because of it.
- **Case 07 needs two nodes on the same fabric**; do not accidentally schedule
  its ranks onto one node when packing cases together, or it silently stops
  testing the cross-transport path.
