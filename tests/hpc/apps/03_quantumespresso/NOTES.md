# NOTES — environment traps, so the next agent does not pay for them again

Everything here cost either a queue slot or a long debugging detour on
2026-08-26/27. Ordered by how much it costs to rediscover.

## MN5 module and launch

**`module load` needs pinned versions and the right order.** This works:

```bash
module load gcc/13.2.0                        # engine needs GLIBCXX_3.4.31
module load nvidia-hpc-sdk/23.11
module load mkl
module load quantumespresso/7.5-nvidia-nvhpcx
```

`module load nvidia-hpc-sdk mkl quantumespresso/...` on ONE line silently loads
nothing — `module list` still shows only `bsc/1.0` and `pw.x` is not on `PATH`.
The QE module declares `prereq("nvidia-hpc-sdk","mkl")` and gives no error.

**Do NOT use `comshu_load_runtime` for QE.** It loads `nvidia-hpc-sdk/25.7`, whose
HPC-X is 2.22.1, while `pw.x` is linked against 23.11's HPC-X 2.16
(`ldd` shows `.../hpcx-2.16/ompi/lib`). The UCX provider is symbol interposition
and must sit in front of the libucp that is actually loaded.

**Interposition across UCX versions is fine.** `libucx_trace.so` carries a
RUNPATH to 2.22.1's `ucx/lib`, but 23.11's module puts 2.16's `ucx/lib` on
`LD_LIBRARY_PATH`, which wins under new dtags. Verified against UCX 1.15 on the
login node — no rebuild needed.

**`srun` does not work; use `mpirun --bind-to none`.** The SDK module prints this
itself: "NVPCX uses its own implementation of MPI that does not offer srun
support." Also `export SLURM_CPU_BIND=none`.

**Forgetting `module load gcc/13.2.0` makes a good capture look empty.**
`comshu read`/`export` die with `GLIBCXX_3.4.30 not found` and the export
directory is simply never created — which reads exactly like "the workload wrote
nothing".

## UCX transport traps

**`UCX_TLS=rc_verbs,...` without `ud_verbs` kills MPI_Init.** RC has no way to
exchange connection data with a peer it is not yet connected to, so UCX wires RC
up over a UD transport. Without it: `PML add procs failed` in `MPI_Init_thread`,
before the application prints a line. Cost: two of four runs in job 45081919.
**This is testable on the login node in seconds** with
`tests/hpc/probes/ucx_api_coverage/mpi_calls.c` and 2 ranks — do that before
spending a slot on any `UCX_TLS` change.

**`rc_verbs` cannot carry a CUDA-aware application's payload.** QE hands MPI GPU
device pointers; UCX's rendezvous must register the user buffer; `rc_verbs`'
memory domain is host-only:

```
ib_md.c:309   ibv_reg_mr(address=..., length=3409920, access=0xf) failed: Bad address
ucp_mm.c:62   failed to register ... on md[3]=mlx5_0: ... (md supports: host)
rndv.c:2360   Assertion `status == UCS_OK' failed
```

What does work: `UCX_TLS=rc_verbs,ud_verbs,self,sm,cuda_copy,gdr_copy` **plus**
`UCX_RNDV_THRESH=inf` (eager through registrable host bounce buffers). That
combination produced 18 700 773 `ib_post_send` rows where the default transport
produced 0.

**hcoll must be disabled or the all-to-all is invisible.** `-mca coll_hcoll_enable 0`.
hcoll implements collectives on its own transport and never calls
`ucp_tag_send_nbx`. Also pin `-mca pml ucx` (ob1 would leave every `ucx_*` table
empty) and `-mca btl ^openib`.

## comshu usage

**`-m simple` is almost always what you want on a real application.**
`ucx_worker_progress` alone was 1 932 658 rows against 125 069 tagged sends — 94 %
of the capture, and nothing any assertion reads. `simple` keeps
post_send/post_recv/modify_qp and every entity (`core/vocab.hpp:79`).

**Add `mlx5dv` when the default UCX transport is in play.** On `rc_mlx5`,
`ib_modify_qp` has NO RTR transition (job 45081919: 70 rows, none carrying
`dest_lid`) because UCX connects through `mlx5dv_devx_qp_modify`.
`-p ib,ucx,mlx5dv` fills `mlx5_devx_qp_connect` (80/160 rows), which
`link_sql.hpp`'s `_owner` view already unions with `ib_modify_qp`.

**A rank that dies by signal loses its low-volume streams entirely.** See
REPORT.md finding 2. Symptom: high-volume event tables are full and
`ucx_context` / `ucx_worker` / `ib_modify_qp` / `node_identity` are missing for
some ranks, so every rank-space join silently returns nothing. If `comm_edges`
is mostly `dst_rank = -1`, check `node_identity` row count against the rank count
**first** — it is not a fabric quirk.

**`pw.x` always exits 139 on MN5.** It reaches `JOB DONE` and then takes SIGSEGV
in teardown. Confirmed with a control run: no comshu, no `LD_PRELOAD`, still 139.
Do not chase it as a comshu bug, and do not treat a nonzero exit as "the workload
failed" — grep for `JOB DONE`.

## The suite harness

**`sql_check.sh`'s `PARAM_COLS` is a fixed allowlist** — `ranks nodes gpus iters
msg_bytes elems elem_bytes app_buffer_bytes regions_per_rank umem_binding
gpudirect rx_depth colls_per_iter p2p_per_iter groups_per_iter`. There is no slot
for a communicator-partition knob like `npool`, so any application whose oracle
is a partition parameter cannot use the shared harness as-is. This case carries
its own `check.sh` for that reason (it also needs to compare two captures, which
`sql_check.sh` cannot do). Spelling `npool` as `groups_per_iter` would have made
the assertions unreadable and the "must reference `params`" rule unverifiable.

**`-- requires:` must distinguish three states, not two.** A table that is
missing, a table that exists but is EMPTY, and a `params.<col>` that is absent
are all "cannot evaluate", and none of them is a failure. On the default UCX
transport `comm_edges` and `ib_post_send` are legitimately empty, and an
assertion over them is *unanswerable*, not false. `check.sh` here reports
`[skip] (... is EMPTY — unanswerable, not failed)`.

## Testing for managed memory — read this before you try

**Symbol interposition does not work on the NVHPC stack.** An `LD_PRELOAD` on
`cudaMalloc`/`cudaMallocManaged`/`cuMemAlloc_v2`/`cuGetProcAddress` measured
**zero for every counter** on a run with 3.73 s of GPU time. The library loaded
(its constructor ran); its symbols were never reached. NVHPC's OpenACC runtime
does not reach the CUDA allocators through the dynamic symbol table, and CUDA 12
hands out driver entry points through `cuGetProcAddress`. Reading that zero as
"no managed memory" would be a false positive.

**Use the link line instead.** `-gpu=managed` and `-gpu=unified` each add exactly
one library, `libnvhpcman.so`; nothing else does. Establish that ground truth by
compiling a three-line OpenACC program both ways on the login node (30 seconds),
then `ldd` the binary in question. `libnvhpcatm.so` is **atomics**, not managed —
it is linked by the MPI stack on every build and means nothing here.

## Cheap login-node work that should precede any slot

All of these cost nothing and each one saved or would have saved a job:

- `nm -D --undefined-only` on `mca_pml_ucx.so` / `libuct_ib.so` — this is how
  REPORT.md finding 3 was found, with no allocation at all.
- `ldd` on the application binary, for both the MPI it will load and the
  compiler runtime libraries it does or does not carry.
- A 2-rank `mpirun` on the login node to validate any `UCX_TLS` string.
- `comshu run --dry-run` to confirm a provider set composes into one pass.
- `bash -n` on the job script — and note that `pw.x_path=...` is not a legal
  bash variable name, which `bash -n` does catch.

## Re-running the checks without a queue slot

The CSV exports persist under `$HOME/comshu-suite/`. Assertions can be iterated
entirely on the login node:

```bash
module load gcc/13.2.0 nvidia-hpc-sdk/23.11
bash tests/hpc/apps/03_quantumespresso/check.sh \
     ~/comshu-suite/qe-npool1-<jobid>.csv \
     ~/comshu-suite/qe-npool2-<jobid>.csv \
     ~/comshu-suite/qe-npool1-rcverbs-<jobid>.csv \
     tests/hpc/apps/03_quantumespresso
```

Do this before submitting anything. Every assertion in this case was debugged
that way after the one job that produced the captures.

## Inputs

AUSURF112 comes from `github.com/QEF/benchmarks` (`ausurf.in` +
`Au.pbe-nd-van.UPF`, 316 073 B). MN5 has no outbound network — fetch locally and
`rsync` to `~/qe-app/`. The benchmark already ships `electron_maxstep = 2`, and
QE prints "convergence NOT achieved" and returns normally rather than calling
`errore`, so it does not need modifying to keep the run short. `K_POINTS
automatic 2 2 1 1 1 0` gives 2 irreducible k-points, which is what makes
`-npool 2` legal — a smaller k-point set would make the whole comparison
impossible.
