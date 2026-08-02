# PLAN — get a suite run under ~5 minutes

- **Created:** 2026-07-30 19:30 +03
- **Status:** DONE 2026-08-02. Step 1 landed; step 3 landed as `--quick` after
  its cheap option turned out not to exist. Step 2 was not needed and was not
  done — see below.
- **Was:** ~25-30 min wall clock, 7 jobs.
- **Target:** ~5 min of job time once scheduled. Queue wait is outside our
  control, so measure and report the two separately — conflating them makes the
  improvement unmeasurable.

## Result (measured 2026-08-02, jobs 44066558 and 44066754)

| run | queue wait | **job time** | cases |
|---|---|---|---|
| full, one allocation (44066558) | 395 s | **9 min 52 s** | 8/8 PASS |
| `--quick` (44066754) | 583 s | **2 min 57 s** | 7 PASS + 1 SKIP |

The `--quick` job is inside the target. The full run is not, and cannot be:
444 s of its 592 s is case 05's fixed NVBit cost, and everything else together
is 136 s. Eight jobs became one.

**The walltime-tuning reasoning did not survive contact with the scheduler.**
The driver asks for 15 min instead of 40 when case 05 is absent, on the theory
that a smaller request is scheduled sooner on a debug QoS. In this sample the
shorter request waited *longer* — 583 s vs 395 s. One pair of runs is not a
measurement of queue behaviour, so the code keeps the smaller request (it cannot
hurt) but no claim is attached to it.

**Per-case times are noisy and should not be read as a profile.** Across the two
runs, case 02 was 36 s then 8 s and case 06 was 23 s then 82 s — same binaries,
same allocation shape. Only case 05's ~440 s has been shown to be stable.

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

## Step 1 — one allocation for the whole suite (biggest win, low risk) — **DONE**

`suite/all_in_one.sbatch`, now the default path in `run_on_mn5.sh`; `--per-case`
restores the old loop for isolating a case.

Each case's `job.sbatch` runs **unmodified** inside the allocation. Its `#SBATCH`
lines are comments, so `bash cases/NN/job.sbatch` is the same program with the
same srun/mpirun shapes as a standalone `sbatch` — nothing was forked into a
suite-only variant that could drift from what a lone submit does. The allocation
is the union of what the cases ask for: 2 nodes x 4 tasks x 4 GPUs.

The hazard this plan flagged — a combined job dying partway and leaving later
cases unreported — was **tested before the run, not asserted**: a fake output
tree with a full green board from an older job id, plus a new manifest whose
job only produced the first three cases, must report five `no result` and
exit 1. It does. `--quick`'s SKIP path was tested the same way.

Submit **one** job with 2 nodes x 4 GPUs and run every case inside it. All seven
fit that shape: 01/02/07 need two nodes, the rest need one. This removes six
queue waits and six job startups outright.

Keep the per-case `result.json` contract exactly as it is, so `run_suite.sh
check` and the manifest keep working unchanged.

*Expected:* ~9 min of job time (the sum above), from ~25-30 min wall clock.
*Acceptance:* same verdict board, one job id.

## Step 2 — run the independent cases concurrently — **NOT DONE, and shouldn't be**

Its own expected value said so: total ≈ slowest case ≈ case 05 ≈ 440 s, so it
buys nothing while 05 is in the run, and nothing at all once 05 is out (the
other seven total 164 s). It would add real risk — concurrent cases sharing
`PCOMM_OUT_ROOT`, GPU and HCA assignment collisions, case 07 needing two nodes
— for no measured gain. Left undone deliberately.

Original sketch below, if the case mix ever changes enough to revisit.


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

- ~~**Test NVBit lazy loading.**~~ **Dead, and it cost nothing to find out.**
  `NO_EAGER_LOAD=1` is a no-op here: NVBit 1.7.7 already defaults to lazy. Its
  own banner string says so — `strings libnvbit.a` gives *"lazy module loading
  is now turned on by default. If you see deadlocks in your tool, set
  NO_EAGER_LOAD to 0"* — so the knob exists to force **eager**, and the only
  direction it can move this run is slower. NVBit sets `CUDA_MODULE_LOADING`
  from it, which is the mechanism. The 440 s is paid *with* lazy loading already
  on. No job was spent on this.
- **Reduce what NVBit must process.** The cost tracks NCCL's 223 specialisations
  in one module, not our filter (438 s filtered vs 436 s unfiltered). Check
  whether a smaller NCCL build, or `NCCL_ALGO`/`NCCL_PROTO` at *build* level,
  yields fewer device functions. Unknown, possibly not available.
- **Tier it out of the run.** **Done, as opt-in `--quick` — not as the default.**
  2 min 57 s of job time, target met. The plan proposed making it the default;
  that was the wrong call. Skipping 05 stops checking NCCL under NVBit, and a
  default that quietly drops coverage is the same failure this project keeps
  finding: a green board that means less than it looks like it means.

  So the loss is made visible instead of hidden. `--quick` writes `SKIPPED` into
  the run manifest, the board prints `SKIP  not run (--quick) — this path went
  unchecked`, and the run ends with `!! INCOMPLETE: 1 case(s) skipped`. Naming
  it in the manifest also matters mechanically: an unnamed case would fall
  through to `check`'s newest-wins lookup and inherit an *older run's PASS*.

## Step 4 — trim the rest only if still needed

Case 02 (~30 s) is the next largest and is pinned to 200k iterations for
determinism. Reducing iterations changes its goldens, so it needs the two-run
gate again — only worth doing if steps 1-3 land and we are still over.

## Still open

- **Case 05's 444 s is untouched**, so a *full* run is ~10 min of job time, not
  5. The only remaining lever is the second option above — fewer NCCL device
  functions for NVBit to process — which needs an NCCL we build rather than the
  SDK's, and may not be reachable at all.
- **Queue wait is now the larger half of a `--quick` run** (583 s vs 177 s).
  Nothing in this plan addresses it and nothing here should: it is the
  scheduler's, and the numbers above keep it separate for exactly that reason.

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
