# NOTES — environment traps, so the next agent does not pay for them again

## GROMACS on MN5

**The module cannot be loaded in one `module load` command.** lmod evaluates
`gromacs/2025.2-gcc-nvhpcx`'s `prereq("fftw/3.3.10-gcc-nvhpcx")` against what is
loaded *at that moment*, so

```
module load nvidia-hpc-sdk/25.7 fftw/3.3.10-gcc-nvhpcx gromacs/2025.2-gcc-nvhpcx
```

fails the prereq and rolls the whole transaction back — **silently**. `module
list` afterwards shows only `bsc/1.0`, PATH is unchanged, and `which gmx_mpi`
comes back empty with no error anywhere in the output. One module per command:

```
module load nvidia-hpc-sdk/25.7
module load fftw/3.3.10-gcc-nvhpcx
module load gromacs/2025.2-gcc-nvhpcx
```

**The module sets a dead environment variable.**
`setenv("GMX_ENABLE_DIRECT_GPU_COMM","1")` — that variable has not existed in
GROMACS since 2022. 2025.2 reads only `GMX_DISABLE_DIRECT_GPU_COMM`, and direct
GPU communication is ON by default. Unset the old one so a job cannot be
misread as having enabled something.

**`gmx_mpi` is linked against an MPI two SDK releases older than the one on
PATH.** `readelf`/`ldd` on `/apps/ACC/GROMACS/2025.2/GCC/NVHPCX/bin/gmx_mpi`:

```
libmpi.so.40 => /gpfs/apps/MN5/ACC/NVIDIA-HPC-SDK/23.11/.../hpcx/hpcx-2.16/ompi/lib/libmpi.so.40
```

but `pcomm_load_runtime` loads `nvidia-hpc-sdk/25.7`, whose `mpirun` is HPC-X
2.22.1. Launching with the 25.7 `mpirun` mixes two Open MPI releases in one job.
Put HPC-X 2.16's `ompi/bin` on PATH and set `OPAL_PREFIX` to it.

**This matters more for the tracer than for the app.**
`libucx_trace.so` carries a RUNPATH to 25.7's `hpcx-2.22.1/ucx/lib`. Because the
provider is `LD_PRELOAD`ed it is resolved *first*, so its `libucp.so.0` wins the
SONAME and Open MPI 4.1.5's `pml_ucx` would then be running against UCX 1.18
instead of the 1.15 it was built for. New dtags mean `LD_LIBRARY_PATH` beats the
baked RUNPATH, so prepending `hpcx-2.16/ucx/lib` fixes it. The job script prints
`ldd libucx_trace.so | grep libucp` for exactly this reason — if that line names
2.22.1, the capture is against a different UCX than the application's.

**The module banner contradicts a memory entry.** It says *"NVHPCX uses its own
implementation of MPI that does not offer srun support. Programs compiled with
NVHPCX must use `mpirun --bind-to none` instead of `srun`"*, and asks for
`export SLURM_CPU_BIND=none`. The `pcomm-mn5-hpcx-nvshmem-env` memory entry says
to launch with `srun --mpi=pmix`. For **this** module (Open MPI 4.1.5 out of SDK
23.11) `mpirun` is what works; the memory entry is about the 25.7/NVSHMEM stack.

**`gmx_mpi -version` hangs on a login node.** It calls `MPI_Init`, and MPI init
never returns on `alogin2` (killed at 120 s under `timeout`, zero bytes of
stdout, zero of stderr — not even the GROMACS banner). Do not use it as a smoke
test; check the binary with `ldd`/`readelf` on the login node and do the runtime
check inside the allocation. The job script's `-nsteps 0` smoke run is that
check, and it costs about a minute.

## Input

benchRIB from <https://www.mpinat.mpg.de/grubmueller/bench> — fetched locally,
`rsync`'d to MN5 (which has no outbound network). 120 MB `.tpr`, md5
`f16f0e021ef1a2c43ad21800943b2d45`, 2,136,412 atoms.

**`natoms` is readable without GROMACS.** The `.tpr` header is XDR: a
big-endian length-prefixed version string, then a run of `int`s, and `natoms`
sits at byte offset 72. `struct.unpack('>I', open(tpr,'rb').read(76)[72:76])`
gives 2136412 for benchRIB and 81743 for benchMEM, both matching the published
figures. That is how the volume assertion gets its oracle without a login-node
`gmx dump`.

**The file is tpx version 83 (GROMACS 4.6 era) and 2025.2 still reads it.**
`tpxio.cpp:252` sets `tpx_incompatible_version = 57`, so anything from GROMACS
4.0 onward is fine. Worth knowing before rejecting a decade-old benchmark set.

## The suite harness

**`suite/sql_check.sh` cannot express this case's oracle.** It materialises
`params` from a hard-coded column list —

```
PARAM_COLS="ranks nodes gpus iters msg_bytes elems elem_bytes app_buffer_bytes
            regions_per_rank umem_binding gpudirect rx_depth colls_per_iter
            p2p_per_iter groups_per_iter"
```

— and `exit 2`s on any `workload` key outside it. Those fifteen names are the
vocabulary of the microbenchmarks the suite was written beside: an iteration
count and a message size. A real application's oracle is `npp`, `npme`,
`nsteps`, `nstlist`, `natoms`, and **not one of them can be written down**. The
only ways out are to hide the oracle inside the SQL as bare literals — which is
the "golden wearing a costume" the suite's own README forbids — or to bring a
runner.

This case ships `asserts.sh`: the assertion half of `sql_check.sh` with the
column list taken from `meta.json` instead of from the script. Same contract
(offending rows, `-- requires:`, `expect_fail_asserts`), no goldens. If more
application cases land, promoting that one change into `sql_check.sh` is the
obvious fix — the column list wants to come from the case, not from the harness.

`asserts.sh` also takes a **variant** argument, which overlays a key from
`meta.json`'s `variants` object onto `workload`. That is what lets one
assertion file be the specification for both arms of a config-knob comparison:
the arm is a parameter (`gpudirect`), not a second query nobody can diff against
the first.

## Things worth doing before spending a slot

- Dry-run the assertions against a **synthetic** CSV export locally. Six
  assertions, six negative controls (flip the variant; delete one rank's PME
  traffic; add a fat non-neighbour edge; add a same-node verbs edge; blank out
  the device ordinals) — all six fired. That cost minutes and would otherwise
  have cost a queue slot each.
- `bash -n` the job script. Two real bugs were caught before submission by
  reading it back: `-nsteps 1000` cannot be removed from a bash array with
  `${arr[@]/-nsteps 1000/}` because it is two elements, and
  `GMX_DISABLE_DIRECT_GPU_COMM= cmd` **disables** direct comm — `getenv()`
  returns non-NULL for the empty string. Unset it, do not blank it.
- `mpirun -x VAR` on an unset variable is a warning per rank. Build the `-x`
  list per run instead of using a constant one.

## What the captures cost, and what they are worth

Job 45082564: four mdrun invocations (bare on/off, traced on/off) plus both
exports and both assertion runs in **4 min 13 s** of a 2-node allocation. Job
45088478 (the mlx5dv probe, 200 steps) was minutes more. A config-knob
comparison genuinely does fit in one slot; the expensive part was deciding what
to pin so the two arms were the same experiment.

`pcomm run -p ib,ucx` cost **0.7 - 1.1 %** of ns/day on a 2.1 M-atom run. That is
low enough that the performance half of a claim can be measured under the tracer
and still be believed.

## Traps specific to tracing an HPC-X application

**Do not expect any verbs traffic.** `nm -D --undefined-only` on both
`libucp.so.0` and `libuct_ib.so.0` finds **zero** references to `ibv_post_send`.
`ib_post_send`, `ib_poll_cq` and therefore `comm_edges` will be empty, and that
is structural, not a misconfiguration. Adding `-p mlx5dv` gets you the DEVX
control plane (`mlx5_umem`, `mlx5_devx_qp`, `mlx5_uar`) but **not**
`mlx5_device_send` — it decodes device-initiated queues, not the ones UCX drives
from the host. Budget your claims accordingly: on this stack the `ucx` provider
is the only one that sees traffic.

**Do not expect the UCX rank graph to work on Open MPI 4.1.x.**
`nm -D --undefined-only mca_pml_ucx.so | grep ucp_worker` prints exactly one
line: `U ucp_worker_query`. The provider hooks `ucp_worker_get_address`, which
Open MPI 4.1.5 never calls, so `ucx_worker_address` records the wrong one of the
two workers each rank creates and the `ucx_ep_connection` cross-rank join
resolves 64 of 117 endpoints, none of them the one carrying traffic. Any
assertion that needs a *peer* will fail; assertions that need only `rank` and
`count` are fine. Write the rank-level form of your oracle as well as the
edge-level one — that is the difference between this case having a result and
not having one.

**`du -sh` on GPFS lies about a directory you just wrote.** The job's own
`du -sh` reported a freshly written parquet tree as `9.5K`; `du -sb` on the same
path minutes later reported 6,559,602 bytes, and every file was present all
along. If a job's closing size report says something collapsed, re-measure
before believing it — that one nearly cost a wrong headline.

**`UCX_LOG_LEVEL=info` did not produce the GPUDirect line.**
`uct_ib_check_gpudirect_driver()` prints `"%s: %s GPUDirect RDMA is %s"` (the
string is in `libuct_ib.so.0`), but it did not appear at `info` on this build.
`ucx_info -c` from a compute node does work and reports
`UCX_IB_GPU_DIRECT_RDMA=try`, `UCX_IB_REG_METHODS=rcache,direct`. Next time try
`UCX_LOG_LEVEL=diag` or `UCX_PROTO_INFO=y` if you need UCX to state which
protocol it picked.

## GROMACS run flags that make two runs comparable

`-notunepme` (PME tuning shifts the cutoff/grid split over the first hundreds of
steps), `-dlb no` (DLB resizes DD cells, so halo sizes drift), `-nstlist 100`
(otherwise GROMACS retunes it from measured performance — and the tracer changes
the measurement). Without these three, the two arms are different experiments
and any byte-level comparison is noise.

**Do not pass `-update gpu`.** With direct GPU comm disabled, GPU update under
domain decomposition is unsupported and an explicit request is a *fatal error*,
so the OFF arm would simply not run. `-update auto` is what a real user gets. On
benchRIB it lands on CPU update in **both** arms anyway (virtual sites, triangle
constraints, coupled-constraint count — `trace-on.log:335`), which is a bonus:
it means the 19 % difference is attributable to communication alone.
