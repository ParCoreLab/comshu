# Quantum ESPRESSO 7.5 (`pw.x`) under pcomm — report

**Machine:** MareNostrum 5, 4 nodes × 1 rank × 1 H100, NDR InfiniBand.
**App:** `quantumespresso/7.5-nvidia-nvhpcx` (NVHPC 23.11, HPC-X 2.16, UCX 1.15,
OpenMPI over UCX). **Input:** AUSURF112 from `github.com/QEF/benchmarks`,
unmodified — 112 Au atoms, `nbnd=800`, `ecutwfc=25`, `ecutrho=200`,
`electron_maxstep=2` as the benchmark ships it. **Runs:** ~12 s each.
**Jobs:** 45081919, 45082487 (both partial, see NOTES.md), **45088258** (final —
every number below comes from it unless stated).

---

## The five findings, in order of value

| # | finding | kind |
|---|---|---|
| 1 | The comm matrix's block structure follows `-npool` exactly, and a transpose message size derived from QE's *own log* appears in the capture to the byte, in both configurations | claim 1 + 3 |
| 2 | **An unclean exit silently destroys rank attribution while preserving 100 % of the event volume.** 15.68 M of 18.70 M sends land on `dst_rank = -1` | claim 2 |
| 3 | **`ucx_worker_address` is never written for any OpenMPI point-to-point application.** The hooked door is `ucp_worker_get_address`; `mca_pml_ucx.so` uses `ucp_worker_query` | claim 4 |
| 4 | QE does **not** allocate managed memory. `plans/todos.txt` item 1 stays latent — with the evidence, and with a warning about how *not* to test it | clean negative |
| 5 | On the default transport pcomm sees no wire traffic at all for this app; one documented UCX setting makes 18.7 M sends visible | claim 4 + reach |

---

## 1. The communicator hierarchy is an oracle, and the capture agrees

### The prediction, written before the first capture

QE partitions `MPI_COMM_WORLD` by arithmetic, not by discovery:

```
Modules/mp_pools.f90:73   nproc_pool = parent_nproc / npool
Modules/mp_pools.f90:79   my_pool_id = parent_mype  / nproc_pool
Modules/mp_pools.f90:86   mp_comm_split(parent_comm, my_pool_id, ...) -> intra_pool_comm
Modules/mp_bands.f90:90   nproc_bgrp = nproc_pool          (nbgrp = 1 here)
```

so `pool(r) = r / (ranks/npool)`, integer division, and pools are **contiguous
blocks of ranks**. `intra_bgrp_comm` is the communicator the FFT descriptor is
built on, and the FFT transpose — the dominant primitive in plane-wave DFT — runs
on sub-communicators of it:

```
FFTXlib/src/fft_scatter.f90:715, :792   mpi_alltoall(..., desc%comm3)
FFTXlib/src/fft_scatter.f90:433, :531   mpi_alltoall(..., desc%comm2)
```

With `ranks = 4`: `npool=1` gives one pool `{0,1,2,3}` and the transpose spans
every rank; `npool=2` gives `{0,1}` and `{2,3}` and the transpose **cannot leave
the pool**. That prediction names no rank and was committed to `job.sbatch` and
to `asserts/` before the run.

### Result A — the fan-out, in bytes

`asserts/endpoint_fanout_by_bytes.sql`, **PASS on both captures.** Heavy
endpoints per rank (>= 10 % of the rank's tagged-send bytes):

| | predicted `ranks/npool - 1` | measured, every rank |
|---|---|---|
| `npool=1` | 3 | 3 |
| `npool=2` | 1 | 1 |

Busiest-endpoint share of a rank's bytes, min–max over the four ranks:

| | by bytes | by messages |
|---|---|---|
| `npool=1` | 0.345 – 0.376 | 0.346 – 0.369 |
| `npool=2` | **0.991 – 0.997** | 0.676 – 0.784 |

`asserts/knob_changes_the_matrix.sql` (**PASS**) asserts only the *order* of
those two, because the magnitudes depend on how many Davidson steps ran, which is
data-dependent. The order is fixed by the decomposition.

### Result B — the transpose message size, from QE's own log

This is the strongest claim here. `pw.x` prints, before the SCF loop starts:

```
sticks:   dense  smooth     PW
Sum       11837    5915    1537
Dense  grid:  2158381 G-vectors     FFT dimensions: ( 180,  90, 288)
R & G space division:  proc/nbgrp/npool/nimage =  4     (or 2 at npool=2)
```

`fft_scatter.f90` builds each peer's all-to-all buffer as `nr3px * ncpx`
`MPI_DOUBLE_COMPLEX` (16 B), with `nr3px = nr3/nproc_rg` and
`ncpx = ceil(sticks_total/nproc_rg)`, and `mp_pools.f90:73` fixes
`nproc_rg = ranks/npool`. So:

```
bytes = (288 / nproc_rg) * ceil(11837 / nproc_rg) * 16
```

| | `nproc_rg` | predicted | in the capture |
|---|---|---|---|
| `npool=1` | 4 | (288/4)*2960*16 = **3 409 920 B** | present, 336 times |
| `npool=2` | 2 | (288/2)*5919*16 = **13 637 376 B** | present, 108 times |

`asserts/fft_transpose_message_size.sql`, **PASS on both.** Neither input is read
off a capture: `288` and `11837` are printed identically at both rank divisions
because they are fixed by the cell and `ecutrho`, so what changes between the two
captures is *derived*, not measured. The knob's effect on the payload is a clean
4x — exactly `(4/2)^2`, the two-dimensional scaling the pencil decomposition
implies — and the per-peer message count halves (9 416 -> 4 696), which is the
k-points-per-pool ratio 2->1.

`count` is bytes here, and that is cross-checked rather than assumed: job
45082487's failure printed `ibv_reg_mr(..., length=3409920, ...)` for the same
buffer this capture records as `count=3409920`.

### What this demonstrates

No microbenchmark in `tests/hpc/cases` can make this claim, because we always
knew its answer in advance. Here the tool predicted a number from a third party's
source and log, and the capture produced it to the byte, twice, under a knob
change. That is claim 3 of `apps/README.md`.

---

## 2. An unclean exit destroys rank attribution and keeps the firehose

**This is the most serious finding and it is not specific to QE.**

`pw.x` reaches `JOB DONE`, prints `FORTRAN STOP`, and *then* takes `SIGSEGV`
during teardown. Attributed with a control run (`bare-clean`, job 45088258): it
happens with **no pcomm and no preload**, so it is the application's own teardown
on this stack, not something we injected.

The consequence in the capture is severe and asymmetric:

```
ib_post_send                18 700 773 rows      <- the firehose survives
ib_modify_qp                        94 rows      <- the join keys do not
node_identity                        2 of 4 ranks

comm_edges (rc_verbs capture, 4 ranks — a complete graph would be 12 edges):
  src  dst        sends       bytes
   0   -1     4 726 783   38 805 728 197
   1   -1     3 214 246   26 418 218 002
   1    3     1 509 623   12 396 270 570
   2   -1     4 722 687   38 803 698 222
   3   -1     3 018 633   24 791 624 251
   3    1     1 508 801   12 395 850 702
```

**15.68 M of 18.70 M sends (84 %) are unattributed**, and only one of six rank
pairs resolved. The mechanism: low-volume streams sit in their buffers until
process exit, so a rank that dies by signal never writes `ib_modify_qp` or
`node_identity` — and rank-space attribution depends *entirely* on those
low-volume streams. `link_sql.hpp`'s `_owner` needs `src_lid <> 0` rows from
every rank; two ranks had none.

The same mechanism was visible in job 45081919 from the other direction: ranks 1
and 3 recorded 28 671 `ucx_tag_send` rows each and **zero** `ucx_context`,
`ucx_worker`, `ucx_endpoint` and `ucx_ep_connection` rows.

pcomm is honest about the result — `dst_rank = -1` is kept deliberately so the
matrix cannot lie about its totals, and `link_health.unresolved_sends` counts it.
The defect is upstream of that: there is **no signal-time flush**. A grep of
`libpcomm/src/engine.cpp`, `pcomm/run.cpp` and `rec/` finds no `sigaction` and no
`SIGSEGV` handling; the only exit path is `atexit`, which a signal bypasses.

Why this matters beyond QE: the failure is silent and inverted. A capture that
lost its join keys looks *bigger* and healthier than one that did not — 18.7 M
rows of real traffic — and the loss shows up only as `dst_rank = -1`, which a
reader can easily take for a fabric quirk. Any real application that aborts,
times out, or is cancelled by SLURM after doing its work lands here.

*Not fixed — reported, per the hard rules.*

---

## 3. `ucx_worker_address` is never written for MPI point-to-point

`ucx_records.hpp:275-279` says of `UcxEpConnection` that joining
`remote_prefix_hash` to another rank's `UcxWorkerAddress.prefix_hash` "is what
turns a pile of per-rank endpoint rows into a communication graph, and it is the
only thing in the UCX records that crosses a rank boundary at all."

In every capture here, `ucx_ep_connection` has rows (8) and
**`ucx_worker_address` has none**. The join has nothing to close on.

Named to a symbol, from a login-node ELF sweep of HPC-X 2.16 — no allocation
required:

| library | `ucp_worker_get_address` | `ucp_worker_query` |
|---|---|---|
| `mca_pml_ucx.so` — **every MPI_Send and MPI_Alltoall** | **absent** | present |
| `mca_osc_ucx.so` — MPI one-sided | present | present |
| `mca_spml_ucx.so` — OpenSHMEM | present | present |

`pml_ucx` publishes its worker address through `ucp_worker_query` with
`UCP_WORKER_ATTR_FIELD_ADDRESS` (its undefined symbols also carry
`ucp_worker_release_address`, so it certainly obtains one). pcomm hooks only
`ucp_worker_get_address` — `providers/ucx/ucx_hooks.hpp:181`,
`ucx_provider.cpp:345`. **The hooked door is the one-sided/OpenSHMEM door.**

So the UCX provider cannot produce a communication matrix for any OpenMPI
point-to-point application, which is most science applications on this machine.
This is the same shape of defect case 08 exists for — an API hooked in one of its
two spellings, green everywhere until a real application picks the other.

Recorded as `asserts/ucx_peer_matrix_is_recoverable.sql`, marked
`expect_fail_asserts` with this evidence. It is the reason every rank-space claim
in finding 1 is stated over *endpoints* rather than peer ranks: those assertions
were deliberately built to need only the local side.

---

## 4. Managed memory: QE does not use it, and here is how not to test that

`plans/todos.txt` item 1 asks whether `mem_object.device` is a lie for migrating
allocations. **For QE on MN5 the question does not arise**, and the defect stays
latent. Three independent lines, none of which is a capture:

**(a) Ground truth for the discriminator.** On MN5's `nvfortran 23.11`, compiling
one OpenACC program three ways:

| flags | links `libnvhpcman.so` |
|---|---|
| `-acc -gpu=cc90,cuda12.3` | no |
| `-acc -gpu=cc90,cuda12.3,managed` | **yes** |
| `-acc -gpu=cc90,unified` | **yes** |

`libnvhpcman.so` is NVHPC's managed allocator — it exports
`__pgi_generic_malloc_managed`, `__pgi_managed_*`, `__man_malloc`, and its
companion `libnvhpcmanaux.so` interposes `malloc`/`free`.

**(b) The shipped binaries.** `ldd` on all four QE builds on MN5:

| binary | `libnvhpcman.so` |
|---|---|
| QE 7.5 `pw.x` | **not linked** |
| QE 7.4, 7.3.1, 7.2 `pw.x` | **not linked** |

(`libnvhpcatm.so` *is* linked and is a red herring — its exports are
`__atomic_*`; "atm" is atomics, not "allocate to managed".) `nm -D` on `pw.x`
shows no `cudaMallocManaged` and no `cuMemAllocManaged`; the only allocation
symbol is `cudaMallocHost`, and the OpenACC data path is `__pgi_uacc_alloc`.

**(c) The source.** QE 7.5 at tag `qe-7.5` contains **zero** occurrences of
`attributes(managed)` or `cudaMallocManaged` — the four hits for "managed" are
English prose in comments. The GPU port is OpenACC (230 files with `!$acc`),
with 9 files using CUDA Fortran `attributes(device)` and 1 using `omp target`.
The historical CUDA-Fortran-managed port the brief anticipated is gone.

### The warning, which is the transferable part

The obvious runtime check **does not work on this stack**, and would have
produced a confident wrong answer if read carelessly. An `LD_PRELOAD` interposer
on `cudaMallocManaged` / `cudaMalloc` / `cuMemAlloc_v2` / `cuMemAllocHost_v2`,
run on all four ranks, reported:

```
[managed-probe] MANAGED: cudaMallocManaged=0 cuMemAllocManaged=0 |
                DEVICE:  cudaMalloc=0 cuMemAlloc=0 | PINNED-HOST: 0 0 |
                cuGetProcAddress: total=0 managed-requests=0
```

Every counter zero — **including plain `cudaMalloc`**, on a run whose own log
says `GPU acceleration is ACTIVE` with 3.73 s of GPU time in `fftw`. The probe's
constructor ran (the line exists), so it was loaded; its interposed symbols were
simply never reached. NVHPC's OpenACC runtime does not reach the CUDA allocators
through the dynamic symbol table, and CUDA 12 hands out driver entry points via
`cuGetProcAddress`, which returns a pointer the caller then calls directly.
(The same mechanism is recorded for du04's IBGDA work: `cuGetProcAddress` and
runtime-symbol interposition both measured 0.)

**So "the interposer saw no managed allocations" is not evidence of anything.**
The build-line evidence above is what carries the conclusion.

The constructive half: pcomm's own door *does* see these allocations —
`mem_object` has 408 rows (`npool=1`) and 802 rows (`npool=2`). So if a
managed/device discriminator is ever added for item 2, it belongs where
`mem_object` is already produced (`core/mem_attr.hpp`'s door), **not** in an
allocator-symbol interposer, which on this compiler would record nothing at all.

---

## 5. What pcomm can see of this application, and at what cost

On the **default transport** — QE exactly as a user runs it — pcomm records no
wire traffic whatsoever:

| table | default (rc_mlx5) | `rc_verbs` + `cuda_copy,gdr_copy` + `RNDV_THRESH=inf` |
|---|---|---|
| `ib_post_send` | **0** | **18 700 773** |
| `ib_modify_qp` RTR rows | **0** | present |
| `comm_edges` | **0** | 6 edges (84 % unresolved — finding 2) |
| `ucx_tag_send` | 125 486 | 125 486 |
| `mlx5_devx_qp_connect` | 80 / 160 | — |

Two separate bypasses, both confirmed here on a real application rather than
inferred from job 44635003:

* **send** — `rc_mlx5` builds WQEs and rings the doorbell inline, below every
  interposable verbs symbol.
* **connect** — it also connects queue pairs through `mlx5dv_devx_qp_modify`, not
  `ibv_modify_qp`. Loading `-p ib,ucx,mlx5dv` does recover this
  (`mlx5_devx_qp_connect`, 80 and 160 rows), but with no sends to attribute,
  `comm_edges` stays empty regardless.

Assertions over `comm_edges` and `ib_post_send` are therefore **unanswerable in
this configuration, not false** — `check.sh` reports them `[skip]` with that
reason, and they are kept rather than deleted. `ucx_and_verbs_agree_on_the_peer_set`
is in that class: with no verbs traffic, the two layers are not describing the
same traffic and the question cannot be put.

### The transport finding, kept separate from the workaround

`UCX_TLS=rc_verbs,ud_verbs,self,sm` alone **aborts** QE (job 45082487):

```
ib_md.c:309    ibv_reg_mr(address=0x7fdc44400000, length=3409920, access=0xf) failed: Bad address
ucp_mm.c:62    failed to register address ... on md[3]=mlx5_0: ... (md supports: host)
rndv.c:2360    Assertion `status == UCS_OK' failed
```

That is a fact about reach, not a flag to tune: **`pw.x` is CUDA-aware and hands
MPI GPU device pointers**, UCX's rendezvous protocol must register the user
buffer, and `rc_verbs`' memory domain is host-only. For a CUDA-aware application,
the payload does not traverse a path a verbs-level tracer can register.

Adding `cuda_copy,gdr_copy` and `UCX_RNDV_THRESH=inf` (eager through registrable
host bounce buffers) makes it work and makes the wire fully visible. That is the
**workaround**, and it is deliberately confined to a bonus run that nothing in
finding 1 depends on — the two captures carrying the `-npool` claim run on the
default transport. It does, however, answer a question
`PLAN-20260815-1540` stage 3 could not: the blind spot is the *transport's* WQE
path, not pcomm's hooks, and there is a supported configuration in which the same
application's wire traffic is completely observable.

Also worth noting: `-m simple` cut 1 932 658 `ucx_worker_progress` rows (94 % of
the capture) that no assertion here reads.

---

## Assertion board (job 45088258)

```
[PASS]  endpoint_fanout_by_bytes            [npool=1] [npool=2]
[PASS]  every_rank_reached_the_ucx_layer    [npool=1] [npool=2]
[PASS]  fft_transpose_message_size          [npool=1] [npool=2]
[PASS]  knob_changes_the_matrix             [both]
[xfail] endpoint_fanout_matches_pool_size   [npool=2]   (XPASS at npool=1 — correct, see below)
[xfail] ucx_peer_matrix_is_recoverable      [npool=1] [npool=2]   -> finding 3
[skip]  npool1_alltoall_spans_every_rank    (comm_edges EMPTY — unanswerable)
[skip]  npool2_pool_local_traffic_dominates (comm_edges EMPTY — unanswerable)
[skip]  ucx_and_verbs_agree_on_the_peer_set (comm_edges EMPTY — unanswerable)
```

### The one assertion that was restated, and why

`endpoint_fanout_matches_pool_size` counts **messages**. It passes at `npool=1`
(3 heavy endpoints) and fails at `npool=2`, reporting 2 heavy endpoints where the
decomposition predicts 1. The capture says why, and it is not a threshold
artefact: rank 0's second endpoint carried 2 638 sends whose most frequent
lengths are 0, 4, 8 and 1 024 B, against 7 340 sends of 4 921 600 B on the first.
It is control traffic, heavy in count and negligible in bytes.

So the assertion carried an unstated assumption — that message count proxies data
movement — which this application falsifies. The **independent** reason to prefer
bytes was already in this repository before the capture existed:
`ucx_records.hpp:80-90` states that a UCP count "must never be added to a bytes
total", and `19_ucx_mpi`'s `workload_note` refuses to assert counts for the same
reason. `endpoint_fanout_by_bytes.sql` measures the quantity the decomposition
actually predicts and passes both.

**The original was not edited and not deleted.** Its threshold is unchanged at
10 %; it is marked `expect_fail` with this evidence, and it stays because the
count/byte divergence it exposes is a real property of this stack. The `XPASS` at
`npool=1` is correct and expected — the claim is true there and false at
`npool=2`, and `check.sh` keys `expect_fail` on the assertion name only.

---

## What is not covered

No `gpumem`/`p2p` pass. NVBit costs ~8 min of module processing per run against a
52 MB binary linking cuFFT, cuBLAS and cuSOLVER, and `p2p` is known-buggy
(`plans/PLAN-20260810-0854`). It would not have helped the managed-memory
question anyway: `mem_object` carries no discriminator, so a `gpumem` capture
could not have answered finding 4 — which is exactly why finding 4 is settled at
the link line.
