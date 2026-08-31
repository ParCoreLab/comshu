# PREDICTION — Quantum ESPRESSO 7.5 under pcomm, round 2

**Written 2026-08-29/30, before any round-2 capture exists.** MN5 has been
unreachable since ~17:00 (alogin1 and alogin2 silent on 22/443/80 while
www.bsc.es and github.com answer from the same machine), so nothing here has
been run. Every number below is derived from Quantum ESPRESSO's own source at
tag `qe-7.5`, from the AUSURF112 input deck, or from round 1's *measured* log
values — and each is labelled with which.

Round 1 (`REPORT.md`, job 45088258) is the baseline. This round changes the
**topology**, not the knob, and that is the whole design: round 1 ran 4 nodes ×
1 rank, so every QE pool boundary was also a node boundary and `-npool` could
only move the *rank* matrix. Round 2 runs 2 nodes × 4 ranks so that `-npool`
moves traffic **between transports**, which is the question
`BRIEF-BLACKBOX.md` names as never having been asked of a real application:

> "does `transport_mix` correctly distinguish shared-memory traffic from fabric
> traffic on a real decomposition" was not [answered].

---

## 0. The one-line prediction

With 8 ranks block-placed 4-per-node, `-npool 2` makes each QE pool node-local,
so the FFT transpose — the dominant primitive in plane-wave DFT — **stops
crossing the fabric entirely**, and `transport_mix` must show that as a collapse
in the share of UCT operations carried by an `mlx5`/`verbs` transport, from a
majority to near zero, with **no other input changed**.

---

## 1. The knob, and why it is this one and not another

`-npool`. Not by preference — by elimination, and the elimination is itself a
source-derived finding:

| knob | status on this build | citation |
|---|---|---|
| `-ntg` / `-nyfft` | **illegal.** `mp_startup` calls `errore('mp_startup','No task groups for GPUs')` when `ntg_ > 1` under `__CUDA` | `Modules/mp_global.f90:83-84` |
| `-nband` | legal here. Forced to 1 only under `__CUDA_OPTIMIZED`, which **neither** of QE 7.5's build systems ever defines | `Modules/mp_global.f90:107-111`; `install/m4/x_ac_qe_cuda.m4`, `CMakeLists.txt` (no `-D__CUDA_OPTIMIZED` in either) |
| `-ndiag` | legal, but it partitions a *different* communicator (the ScaLAPACK ortho grid) and its default is a function of `nproc_bgrp`, so it is a **confound** for the npool comparison | `LAXlib/mp_diag.f90:20-34` |
| `-npool` | legal, and the only one whose partition is guaranteed to align with the node boundary | `Modules/mp_pools.f90:73-92` |

`-D__CUDA` is unconditional for any GPU build (`x_ac_qe_cuda.m4:91`,
`try_dflags="$try_dflags -D__CUDA"`), so the `-ntg` prohibition binds on the
`quantumespresso/7.5-nvidia-nvhpcx` module by construction. **This is worth
recording on its own: on a GPU build of QE 7.5 the advertised four-level
parallelism is really two levels.** Round 1 passed `-ntg 1` and never learned
that `-ntg 2` would have aborted.

`-ndiag 1` is pinned in **both** arms so the diff is attributable to `-npool`
alone. That is a change from round 1 (which left `ndiag` at its default) and it
is deliberate: at `nproc_bgrp = 8` and at `nproc_bgrp = 4` the default ortho
grid is the same 2×2, but "the same by arithmetic today" is not an attribution.

## 2. The arithmetic that makes the prediction, not a measurement

QE partitions `MPI_COMM_WORLD` by integer division, never by discovery:

```
Modules/mp_pools.f90:73   nproc_pool = parent_nproc / npool
Modules/mp_pools.f90:79   my_pool_id = parent_mype  / nproc_pool
Modules/mp_pools.f90:86   mp_comm_split(parent_comm, my_pool_id, ...) -> intra_pool_comm
Modules/mp_pools.f90:92   mp_comm_split(parent_comm, me_pool,   ...) -> inter_pool_comm
Modules/mp_bands.f90:90   nproc_bgrp = parent_nproc / nbgrp   (nbgrp = 1 here)
```

so **pools are contiguous blocks of ranks**, and `inter_pool_comm` is the
*strided* complement. The FFT descriptor is built on `intra_bgrp_comm`, and the
transpose runs on its sub-communicators:

```
FFTXlib/src/fft_scatter.f90:433,:531   mpi_alltoall(..., desc%comm2)   xy planes
FFTXlib/src/fft_scatter.f90:433?,:...  (comm2 = y-fft group; nyfft=1 here)
FFTXlib/src/fft_scatter.f90:664,:...   sendsize over desc%comm3        yz planes
```

The job script pins `mpirun --map-by ppr:4:node`, so rank → node is
`node(r) = r / 4`, block placement, fixed by the launcher and not by QE.

Composing the two:

| arm | `npool` | `nproc_bgrp` (= P) | pools | does a pool span nodes? |
|---|---|---|---|---|
| A | 1 | 8 | `{0..7}` | **yes** — 4 of each rank's 7 transpose peers are off-node |
| B | 2 | 4 | `{0,1,2,3}`, `{4,5,6,7}` | **no** — every transpose peer is on the same node |

That is the entire experiment. Nothing in it names a rank; the predicate
`(ranks / npool) > (ranks / nodes)` decides which claim applies, and it is
computed in SQL from `params`.

## 3. What the trace must show — the falsifiable claims

### P1 (headline) `transport_mix` must move with the pool boundary

Let *fabric* mean a `tl_name` matching `%mlx5%` or `%verbs%` — UCX's own names
for its InfiniBand transports (`rc_mlx5`, `dc_mlx5`, `ud_mlx5`, `rc_verbs`,
`ud_verbs`), none of which collide with its intra-node ones (`self`, `posix`,
`sysv`, `cma`, `knem`, `xpmem`, `cuda_ipc`).

* **Arm A**: fabric share of UCT operations **≥ 0.25**. Predicted ≈ 4/7 of
  transpose traffic, so comfortably above.
* **Arm B**: fabric share **≤ 0.10**. Predicted ≈ 0 for the transpose; what
  remains on the fabric is wireup plus the per-SCF `inter_pool_comm` density
  reduction, which is a handful of messages against thousands.
* **Both arms**: total UCT operations > 0 and at least one non-fabric transport
  present. Without this, arm B passes vacuously on an empty capture — which is
  exactly the confusion `apps/README.md` says has cost this project slots.

**Why this is a claim about the tool and not about UCX.** UCX choosing shared
memory for a same-node pair is not news. The claim is that `transport_mix`
*attributes the right bytes to the right transport when the application's own
decomposition is what decides which peers are same-node* — a mapping that runs
QE source → rank arithmetic → placement → UCT interface, four layers deep, and
that no microbenchmark in `tests/hpc/cases` produces because all of them pin one
rank per node precisely to avoid it (`19_ucx_mpi`, `21_ucx_deep` both say so in
their `workload_note`).

**Measured on operations, not bytes, and that is deliberate.** `UctSend.payload`
"means something different per op family — sent, requested, or advertised — so
it must not be summed across them" (`pcomm.fbs`, type 804). QE's messages are
0.8–3.4 MB, far above any default `UCX_RNDV_THRESH`, so they are rendezvous:
the sender *advertises* and the bytes appear in `advertised_bytes`, not
`out_bytes` (`21_ucx_deep/asserts/rendezvous_advertises_rather_than_sends.sql`).
Summing across the split would double-bill. `ops` is the one column that is
safely additive. Bytes are **printed** by the job script per `(scope, tl_name,
op)` and asserted on by nothing.

### P2 The transpose message size, derived from the grid, must be in the capture

`fft_scatter.f90:662-664` builds each peer's chunk as
`sendsize = ncpx * nr3px` elements of `MPI_DOUBLE_COMPLEX` (16 B), with
`nr3px = MAXVAL(desc%nr3p)` and `ncpx = MAXVAL(ncp_)`, i.e. the per-process
maxima of a balanced split of `nr3` planes and of the total stick count over
`P = nproc_bgrp`. So

```
transpose_bytes = (nr3 / P) * ceil(sticks_dense_total / P) * 16
```

`nr3 = 288` and `sticks_dense_total = 11837` are **pw.x's own printed numbers**
for AUSURF112 and are independent of the rank division — round 1 verified that
they print identically at two different divisions, because they are fixed by the
cell and `ecutrho`.

| arm | P | predicted bytes | status |
|---|---|---|---|
| A | 8 | `36 * 1480 * 16` = **852 480** | new |
| B | 4 | `72 * 2960 * 16` = **3 409 920** | **already measured** — round 1 recorded exactly this at 4 ranks / `npool=1`, present 336 times |

Arm B's number reproducing round 1's, at a different rank count and a different
`npool`, is a stronger check than either arm alone: it says the derivation
tracks `nproc_bgrp` and nothing else. It must appear as a `ucx_tag_send.count`.

The `ceil` is an assumption about QE's stick balancing; it was exact at P = 4
and P = 2 in round 1. The bare run in the same allocation prints
`sticks: dense  Max` which **is** `MAXVAL(ncp_)`, so if the assertion fails the
job's own log says whether the formula or the capture was wrong.

### P3 `tag_flow` must close on a real application

Never tested outside our own probes. Two clauses, from
`21_ucx_deep/asserts/tag_flow_closes_the_two_layers.sql`, reused verbatim in
shape:

* if any hardware-tag door fired **and** `ucx_tag_send` is non-empty, at least
  one `tag_flow` row must have `status = 'tag_matched'`;
* a `tag_matched` flow with `doors = 1` and `uct_payload = ucp_units` must have
  `ucp_sends = uct_ops`.

**Predicted outcome: the first clause fails or is vacuous.** MN5's HPC-X 2.16
does not enable ConnectX tag matching by default, and the job deliberately does
not force it — forcing `UCX_RC_MLX5_TM_ENABLE=y` would be tuning the fabric to
make a table non-empty. If there are no tag doors, `tag_flow` has nothing to
join on for this application, and *that* is the finding: **the only join pcomm
has between the 600s and the 800s requires a fabric feature that a real MPI
application does not switch on.** Recorded as claim type 4, named to a symbol.

### P4 `uct_binding` decides whether any of this is answerable

`ucx_deep` patches `uct_iface_ops_t`, guarded on the ops-table size
(`pcomm.fbs` type 806: "`accepted = 0` is why `uct_send` is empty"). QE's module
loads **HPC-X 2.16 / UCX 1.15** — older than anything this provider has been run
against (case 21 measured runtime 1.18.0 against a 1.19.0 build). I cannot
determine offline whether that is accepted; `pcomm list` documents what a
provider captures, not what it requires.

**Predicted: 50/50, and either result is a finding.** `accepted = 0` on UCX 1.15
is claim type 4 — the transport layer is blind on the exact stack the science
applications use, and the version boundary is named. It is also why P1 has a
fallback.

### P5 (fallback for P1, on the 600s only) endpoint fan-out follows the pool

Round 1's `endpoint_fanout_by_bytes` claim, re-run at 8 ranks. Heavy endpoints
per rank, where "heavy" is `≥ 0.5/(P-1)` of the rank's tagged-send bytes — half
the equal share, derived from `params`, not a magic 10 %:

| arm | P | predicted heavy endpoints per rank |
|---|---|---|
| A | 8 | 7 |
| B | 4 | 3 |

This needs only `ucx_tag_send` and `ucx_endpoint`, which round 1 proved are
populated for QE on the default transport (125 486 rows). If `ucx_deep` is
refused, the knob claim still stands on this.

### P6 Managed memory — round 1's negative, independently reconfirmed

`apps/README.md` lists QE as the candidate that would turn `plans/todos.txt`
item 2 from latent to live. **It does not.** Round 1 settled this at the link
line; round 2 adds a line of evidence round 1 did not cite — QE's own build
systems:

* `install/m4/x_ac_qe_cuda.m4:97,119,120,123-125` — the *complete* GPU flag set
  is `-cuda -gpu=cc<CC>,cuda<VER>` plus `-acc`. There is no `-gpu=managed` and
  no `-gpu=unified` anywhere in the file, and those two flags are the only ones
  that link `libnvhpcman.so` (round 1 established that ground truth by
  compiling an OpenACC program three ways on the login node).
* `CMakeLists.txt:61-68,314-321` — the CMake path is the same: `QE_ENABLE_CUDA`
  plus `QE_ENABLE_OPENACC`, no managed-memory option exists to set.
* `x_ac_qe_cuda.m4:92-93` — `-D__GPU_MPI` is defined when `--with-cuda-mpi=yes`,
  which is what makes QE hand MPI *device* pointers. Round 1 proved that flag is
  on for this module the expensive way: `rc_verbs` aborted with
  `ibv_reg_mr(...) failed: Bad address / md supports: host`.

So: QE 7.5 is CUDA-aware and **not** managed. The prediction is that
`mem_object` will again have rows (round 1: 408 / 802) and that **no query over
them can distinguish a managed allocation from a device one**, because
`mem_object` carries no door discriminator — which is `apps/README.md`'s own
statement of the gap, now confirmed against a second application. Nothing in
this study can move item 2; the honest outcome is that QE removes the last
candidate the plan was counting on.

### P7 `rank_identity.device` (testing a sibling's claim, not trusting it)

A sibling reports `rank_identity.device` is NULL and `label` reads `GPU ?` on
any capture without the gpumem provider. I have not verified it and will not
assume it. This job requests `--gres=gpu:4` with 4 ranks per node **so that the
four GPUs are distinguishable** — under round 1's `--gres=gpu:1` every rank sees
device 0 and the claim is untestable.

**Predicted: the assertion FAILS**, i.e. the sibling is right. It is written as
a live assertion and *not* pre-marked `expect_fail`; predicting a failure in
writing beforehand is the honest form, marking it xfail on another agent's word
is not. If it fails, the finding is sharper than the sibling's: this capture
carries a device ordinal in `ucx_tag_send.device` (`pcomm.fbs` type 606) on
every one of ~10^5 rows, so the information is present and the derived table
declines to use it.

### P8 Controls, predicted from round 1 and asserted so a regression is visible

* `ib_post_send` = **0**. `rc_mlx5` rings its own doorbell. Round 1 measured
  this; round 2 re-measures it with a third provider co-loaded.
* `ucx_worker_address` = **0 rows**, so `comm_edges` cannot be built in
  rank space. Round 1 named the symbol: `mca_pml_ucx.so` publishes through
  `ucp_worker_query`, and pcomm hooks `ucp_worker_get_address`. Every claim
  above is therefore stated over **endpoints and transports**, never over peer
  ranks — the same discipline round 1 used, for the same measured reason.
* `pw.x` exits **139** after printing `JOB DONE`. Not a pcomm bug; round 1
  attributed it with an unpreloaded control run. The job greps `JOB DONE`.
* Low-volume streams may be truncated on the ranks that die by signal
  (round 1: `node_identity` 2 of 4 ranks). If `uct_iface` is missing for a rank
  while `uct_send` is full, that is this failure and not a transport bug.

## 4. What would falsify each claim

| claim | falsified by | and that would mean |
|---|---|---|
| P1 | arm A fabric share < 0.25 | `transport_mix` is not attributing inter-node traffic, **or** `tl_name` is not a resolved string in the export |
| P1 | arm B fabric share > 0.10 | the pool is not node-local: either QE's split is not contiguous or `--map-by ppr:4:node` did not take. The bare run's `R & G space division` line settles which |
| P2 | the size is absent | the transpose derivation is wrong, `count` is not bytes for this datatype, or the all-to-all did not reach `ucp_tag_send` (hcoll — disabled) |
| P3 | matched flows disagree on counts | the 600↔800 join is wrong on a real tag stream |
| P5 | heavy count ≠ P−1 | round 1's claim does not survive a change of scale |
| P7 | passes | the sibling's claim is wrong, or it does not apply to a ucx-only capture |

## 5. What this study cannot answer, stated up front

* **Nothing about one-sided operations.** QE's parallel layer (`UtilXlib/mp.f90`)
  is two-sided; `ucx_rma` is predicted empty. The sibling gap "no documented join
  from a one-sided operation to the peer rank" is **not testable on QE** and
  belongs to `05_osu_onesided`. `every_rma_key_resolves-r2.sql` is included
  anyway: it passes vacuously and goes live the day QE surprises us.
* **Nothing from `gpumem`.** ~8 min of NVBit module lift against a 52 MB binary,
  and it could not answer the managed question anyway (no discriminator).
* **No rank-space comm matrix.** See P8.
