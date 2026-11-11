# Real applications — brief for the app-validation agents

Everything under `tests/hpc/cases/` is a workload **we wrote**, to exercise a
door **we chose**. That is what makes the suite a regression net and what makes
it a weak oracle: a microbenchmark cannot surprise the tool that was written
beside it. Three separate items in `plans/todos.txt` are stuck for exactly this
reason — they need behaviour our own workloads have no reason to produce.

This directory is the other kind of evidence: **third-party applications nobody
here wrote, run under `comshu`, with the capture judged against what the
application's own source says should have happened.**

Started 2026-08-26. One agent per application.

---

## The claim each agent is asked to defend

Not "comshu ran without crashing". That is table stakes and is not a finding.
The deliverable is one of these, with evidence:

1. **The capture is right.** Some number the application itself determines —
   a rank count, a halo-exchange partner set, a message size that follows from
   a decomposition, a collective count that follows from the iteration count —
   is independently derivable from the app's source or its own log, and the
   capture agrees. This is the assertion pattern the suite already uses
   (`suite/README.md`, "Two kinds of check"): **an oracle that is not the
   capture itself.**

2. **The capture is wrong**, and here is the smallest reproduction. This is
   worth more than (1). `BUGS-FOUND.md` is where it lands.

3. **The capture explains a difference the application's own documentation only
   asserts.** Run the app twice with one knob changed, diff the two captures,
   and show *in the trace* what the knob did. This is the headline: it is the
   thing a profiler is for, and no microbenchmark in our suite can demonstrate
   it because we always knew the answer in advance.

4. **The tool cannot see this workload, and here is precisely which door is
   missing.** A negative result, named to a symbol. Also valuable — that is how
   `PLAN-20260807-2119` (UCX's context door) and case 08's empty
   `ib_queue_pair` were found.

An agent that finishes with "it worked, N rows captured" has not answered the
question. N rows of *what*, and how do you know N is right?

---

## Hard rules

**Do not edit anything outside your own case directory and your report.** In
particular: no changes to `libcomshu/`, `providers/`, `gpu-backends/`,
`comshu-read/`. If you believe you found a tool bug, that is a *finding* — write
it up with the reproduction. Fixing it is a separate decision by the human, made
after the finding is on the table. Four agents editing the tracer concurrently
would make every capture unattributable.

**Never edit an assertion to match what comshu emitted.** That inverts the test.
`suite/README.md` says why, and the suite has an `expect_fail_asserts`
mechanism for exactly this: record the failure with evidence and let a fix flip
it.

**One MN5 job at a time, cluster-wide.** The `acc_debug` QoS allows a single
submitted job per account, and four agents share one account. Before `sbatch`:

```bash
while squeue -u koc296797 -h | grep -q .; do sleep 60; done
sbatch ...
```

Poll, do not retry blindly. A job that fails to submit because another agent
holds the slot looks identical to a broken script.

**Queue slots are the scarce resource, not your time.** Every job costs a wait
measured in minutes to hours, and four agents are serialising through one slot.
So do not spend a slot on a single question. **Every job script runs the
workload bare first, then again under `comshu`, inside the one allocation** —
`suite/all_in_one.sbatch` is the pattern. The bare run is not optional: a
capture of a workload that did not work is indistinguishable from a tracer that
captured nothing, and that specific confusion has cost this project several
slots already. Put both configurations of your config-knob comparison in that
same job too, if they fit in the walltime.

Everything that does not need a GPU — reading the application's source, deriving
the oracle, writing the assertions, checking module contents with `ldd`/`nm`/
`strings` — happens on the login node while you wait, not in the allocation.

---

## Machines

| | MN5 (`alogin2.bsc.es`, user `koc296797`) | du04 (`ssh -J mktips@100.64.0.8 missa18@172.20.31.90`) |
|---|---|---|
| GPUs | 4× H100 per node, multi-node over NDR IB | 4× A100, single node only |
| queue | SLURM, `acc_debug`, **one job at a time** | none — run directly |
| apps | GROMACS/LAMMPS/QE/NAMD/VASP as **modules** | nothing preinstalled |
| use it for | anything multi-node; anything needing a module | shake-out runs, NVBit iteration, single-node p2p |

`alogin1` refuses SSH often; `alogin2` has the same home directory. Login shell
is **tcsh** — wrap remote commands in `bash -lc "..."`.

MN5 has **no outbound network**. Anything to be fetched is fetched on the local
machine and `rsync`'d over.

du04's `/usr/local/cuda` is **compiler-only** — no `nvdisasm`, which NVBit needs.
Without it NVBit prints an error, **exits 0**, and leaves an empty capture. Use
`/opt/nvidia/hpc_sdk/Linux_x86_64/25.9/cuda/13.0/bin` on PATH.

---

## Running comshu

comshu is already built on MN5 at `$HOME/proj/comshu/build`. Do **not** rebuild it
or run `tests/hpc/run_on_mn5.sh` — that syncs the whole repo and would clobber
another agent's tree. Source the suite environment and use what is there:

```bash
source $HOME/proj/comshu/tests/hpc/suite/common.sh
comshu_load_runtime                       # gcc 13.2 + HPC SDK 25.7 + CUDA on PATH

OUT=$HOME/comshu-apps/<app>-$SLURM_JOB_ID
$COMSHU run -p <providers> -o "$OUT" -- <the application and its arguments>

$COMSHU read   "$OUT"                     # what streams exist, and their row counts
$COMSHU export "$OUT" --csv "$OUT.csv"    # one CSV per record type
comshu_derive  "$OUT"                     # parquet + the derived layer (needs DuckDB)
```

Under MPI, `comshu run` wraps the *ranked* binary, not `mpirun`:

```bash
srun --mpi=pmix ... $COMSHU run -p ib -o "$OUT" -- ./app args      # preferred on MN5
mpirun -np N ... $COMSHU run -p ib -o "$OUT" -- ./app args
```

Each rank writes `$OUT/rank-<N>/`. **A capture directory with no `rank-*`
subdirectory means the workload never started** — read the job's stdout before
reading anything else.

### Which provider

| `-p` | what it records | when it applies |
|---|---|---|
| `ib` | ibverbs: QPs, MRs, `post_send`/`poll_cq`, the wire | anything over InfiniBand, incl. NCCL and UCX underneath |
| `ucx` | UCX API: tag send/recv, RMA, endpoints, workers | anything on HPC-X / OpenMPI-over-UCX. **The science apps.** |
| `nccl` | NCCL host API: collectives, p2p, comms, groups | anything using NCCL directly |
| `mlx5dv` | DEVX / IBGDA control plane | NVSHMEM device-initiated paths |
| `p2p`, `gpumem` | GPU memory accesses via NVBit | **expensive** (~8 min module cost) and `p2p` is known-buggy — see `plans/PLAN-20260810-0854`. Do not start here. |

Providers compose: `-p ib,ucx` is the interesting one for an MPI application,
because it captures the intent *and* the wire and lets you ask whether they
agree. That question — does UCX's `ucp_tag_send` correspond to the
`ibv_post_send` beneath it — is Stage 3 of `PLAN-20260815-1540`, currently
**blocked on research**. A real application is the instrument that plan lacked.

### Reading the capture

Row counts come from `$COMSHU read`. Real analysis is SQL over the derived layer:

```bash
$COMSHU sql "$OUT.pq" "SELECT src_rank, dst_rank, sends, bytes FROM comm_edges ORDER BY bytes DESC"
```

Derived tables: `comm_edges` (rank→rank traffic), `object_access` (which
allocation a GPU access landed in), `wr_flow` (work request → completion
pairing), `timeline_span`, `rank_identity`, `endpoint`, `link_health`.
Raw tables are one per record type — `ib_post_send`, `ucx_tag_send`,
`nccl_collective`, `mem_object`, `gpu_access`, … — see `$COMSHU types`.

Known reader wart: `POST /query` and `comshu sql` reject a statement whose first
token is a comment. Put comments *after* the `SELECT`.

---

## Three open questions a real application may settle

Look for these. They are why this exercise exists, and finding one is a better
outcome than a clean report.

1. **Managed memory** (`plans/todos.txt` item 1). `mem_object.device` is one
   `DeviceId` fixed at allocation, and managed memory migrates by definition —
   so the column silently means "where it started". No workload of ours
   allocates managed memory. **QuantumESPRESSO** built with the NVIDIA HPC SDK
   uses CUDA Fortran managed arrays, and **LAMMPS/Kokkos** has a `CudaUVM`
   backend. If your app uses either, say so — that turns a latent design
   question into a live defect. Note there is currently **no discriminator**:
   `mem_object` does not record which door an allocation came through, so
   evidence has to come from the app's build flags or a `cuMemAllocManaged`
   trace, not from the capture alone.

2. **UCX → verbs attribution** (`PLAN-20260815-1540` Stage 3). `ibv_post_send`
   is not UCX's send door on MN5 (measured, job 44635003) and no replacement has
   been named. With `-p ib,ucx` on a real MPI app you have both layers in one
   capture: if a correspondence exists, its shape is visible here.

3. **Unattributed GPU accesses** (`plans/todos.txt` item 3). 1.18M accesses on
   case 05 land in no tracked object because NCCL's internal channel buffers
   arrive through a door `gpumem` does not record. Only relevant if you run a
   `gpumem` capture, which is expensive — do not do it on the first pass.

---

## What to produce

A directory `tests/hpc/apps/<NN>_<app>/` containing:

- `REPORT.md` — the finding, in the shape of the four claims above. Lead with
  what you found, not with what you did. Quantities with units. Every claim
  either derived from the application's source (cite file:line) or from the
  capture (cite the query). Where they disagree, say which you believe and why.
- `job.sbatch` (or `run.sh` for du04) — reproducible, self-contained, following
  the pattern of `tests/hpc/cases/20_nccl_collectives/job.sbatch`.
- `meta.json` — same schema as a suite case: a `workload` block holding the
  parameters your assertions use as their oracle.
- `asserts/*.sql` — the checks. Same rule as the suite: **an assertion must
  either reference `params`, or name no rank, device or QP number the run
  happened to produce.** If it names an identifier the run produced, it is a
  golden wearing a costume.
- `NOTES.md` — the environment traps you hit, so the next agent does not pay
  for them again. This is not optional; three of the memory entries this
  project relies on started as one of these.

Do not register the case in the suite runner. These are studies, not gates —
promotion to a gate is a later decision, and a flaky third-party app in the
merge gate would be worse than no gate.

## What to write down even if it fails

A job that produced nothing is still evidence, provided you can say **which**
nothing: the app failed to launch, the app ran and comshu captured zero rows,
comshu captured rows and the export failed, or the export worked and the
assertion was wrong. Those four have completely different causes and look
identical in a summary that says "no data". Say which one, with the stdout that
shows it.
