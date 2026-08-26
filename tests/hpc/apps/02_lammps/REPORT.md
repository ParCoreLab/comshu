# 02_lammps — LAMMPS/KOKKOS (CUDA) under pcomm on MareNostrum 5

App: `lammps/10Sep2025` (upstream tag `patch_10Sep2025`), KOKKOS package, CUDA
backend, Kokkos 4.6.2, OpenMPI 4.1.5 → UCX 1.20.0 → NDR InfiniBand.
Run: job **45082034**, 2 nodes × 4 H100, 8 ranks, `bench/in.lj` at 256 000 atoms,
100 steps, `processors 2 2 2`. Six executions in one allocation: two bare, four
captured (`ib,ucx` and `ib`, each × `gpu/aware on|off`).

---

## The findings, in order of value

**1. LAMMPS/Kokkos on MN5 does NOT use managed memory. `plans/todos.txt` item 2
stays latent, and the reason it stays latent is stronger than "no workload of
ours does it".** Four independent lines of evidence, none from the capture.

**2. pcomm cannot name the peer rank of a single LAMMPS message, and the missing
door is one symbol: `ucp_worker_query`.** 0 of 621 halo messages per rank
resolve to a peer. This is new — it is not the `ibv_post_send` gap.

**3. `-pk kokkos gpu/aware on|off` changes the wire by exactly nothing, and the
capture says so to the byte** — identical send counts and identical byte totals
on all 8 ranks, on *both* providers. The 2.03× wall-clock difference is real and
is not in the message layer. This is the claim-(3) result, and §B below
separates it explicitly from the claim-(4) reading it could be mistaken for.

**4. The capture is right about LAMMPS where it can speak at all.** Five derived
assertions pass, including the halo fan-out of 3 and the neighbour-rebuild
period read back off the wire.

---

## A. Managed memory — a clean negative, with the trap that would have faked a positive

`plans/todos.txt` item 2 names LAMMPS/Kokkos as one of two applications that
"would make this live on its first capture". For this build it does not, and the
question is settled without a capture — which is the only honest way to settle
it, since `mem_object` records no discriminator for the allocation door.

### A1 — Kokkos: the switch exists, and it is off

Kokkos 4.6.02, `core/src/Cuda/Kokkos_Cuda.hpp:103-109`:

```cpp
#if defined(KOKKOS_ENABLE_CUDA_UVM)
  using memory_space = CudaUVMSpace;
#else
  using memory_space = CudaSpace;
#endif
```

So `Kokkos::Cuda::memory_space` — and therefore every LAMMPS `View` on the
device — is `CudaUVMSpace` **iff** that one macro is defined. In 4.6 the option
is deprecated and self-defending: `cmake/kokkos_arch.cmake:1315-1321` emits a
`DEPRECATION` and then `FATAL_ERROR`s unless `Kokkos_ENABLE_DEPRECATED_CODE_4`
is *also* set. Turning it on is now a deliberate act, not a default.

### A2 — LAMMPS prints a line when it is on, and the line is not in the binary

`src/KOKKOS/kokkos.cpp:97-101`:

```cpp
#if ((defined(KOKKOS_ENABLE_CUDA) && defined(KOKKOS_ENABLE_CUDA_UVM)) || ...)
  if (me == 0) utils::logmesg(lmp,"  using unified memory\n");
#endif
```

`strings` over `liblammps_mpi.so.0` finds its neighbours from the same `logmesg`
block and not that one:

```
  using double precision
  using double precision FFTW3
  using double precision cuFFT
  using view layout = legacy
"  using unified memory"  ->  occurrences: 0
```

Confirmed at runtime too: the 8-rank bare log carries `using view layout =
legacy` and `will use up to 4 GPU(s) per node`, and no unified-memory line.

### A3 — no LAMMPS View was ever instantiated over `CudaUVMSpace`

`nm -D`, LAMMPS-namespace symbols only:

| module | `LAMMPS_NS` × `CudaUVMSpace` | `LAMMPS_NS` × `CudaSpace` |
|---|---|---|
| `10Sep2025` | **0** | 595 |
| `22Jul2025-update2` | **0** | 507 |
| `7Feb2024` | **0** | 584 |
| `2Aug2023` | **0** | 368 |
| `29Aug2024-update3` | 0 | 0 *(no KOKKOS-CUDA at all)* |

Every KOKKOS/CUDA LAMMPS on this machine, not just the one I ran.

### A4 — and the process, on a GPU, makes no managed allocation

An `LD_PRELOAD` counter over the CUDA allocation doors (built from source inside
`job.sbatch`), all 8 ranks of the bare `gpu/aware on` run:

```
ALLOC_PROBE rank=0 cudaMalloc=231(64855047B) cudaMallocManaged=0(0B) cudaMallocHost=1
            cudaHostAlloc=2 cudaMallocAsync=0(0B) cuMemAlloc=1 cuMemAllocManaged=0
            cuMemAllocHost=0 cuMemAllocAsync=0 cuMemAllocFromPoolAsync=0
... ranks 1-7 identical in shape: cudaMalloc 228-240 (52.7-101.4 MB),
    cudaMallocManaged=0, cuMemAllocManaged=0 on every rank.
```

### The trap, stated because it is the test one reaches for first

```
$ nm -D --undefined-only liblammps_mpi.so.0 | grep -i 'cudaMalloc\|cuMemAlloc'
   U cuMemAllocHost_v2
   U cuMemAlloc_v2
   U cudaMalloc@libcudart.so.12
   U cudaMallocHost@libcudart.so.12
   U cudaMallocManaged@libcudart.so.12      <-- present, and means nothing
```

`cudaMallocManaged` is an import of **every** Kokkos/CUDA binary here, including
the four with zero UVM Views. Kokkos compiles `CudaUVMSpace::impl_allocate`
unconditionally whenever the CUDA backend is on; the symbol is reachable code
that nothing calls. An ELF-import sweep — the same shape of sweep item 2 already
used to find `cuMemAllocManaged` in libnccl and libnvshmem — would have reported
LAMMPS as a managed-memory user. It is not one.

**Verdict.** `uses_managed_memory: 0` in `meta.json`. Item 2's LAMMPS half is
closed: not "we didn't happen to hit it", but "this build cannot hit it, and
making it hit it now requires re-enabling deprecated Kokkos code". The
QuantumESPRESSO half is untouched by this study.

### A5 — a side observation about `mem_object.device`, correctly scoped

On rank 0 of the `ib,ucx` capture, `mem_object` holds 69 objects / 825 462 384 B,
of which **63 objects and 823 017 616 B carry `device = 65535`**, the
unknown-device sentinel `link_sql.hpp` filters on. Only 6 objects / 2 444 768 B
name a real GPU. The large ones are 18 × 39 845 888 B, which is UCX registration
territory, not Kokkos allocation territory (`ib_mem_region` = 775 in the same
capture).

This is **not** presented as a defect. It is an `ib`+`ucx` capture with no
`gpumem` provider loaded, so there is no NVBit-side source for device
attribution and most of what `mem_object` holds is host memory that legitimately
has no device. It is recorded because item 2 is *about* that column, and because
"the field is mostly unset on a real MPI application" is a fact worth having
before anyone designs `ResidesOn` around it.

---

## B. The config knob — claim (3), and the claim-(4) reading it is not

`-pk kokkos gpu/aware on|off`. What the source says must happen:
`src/KOKKOS/kokkos.cpp:615-641` — with `gpu/aware off` and `nmpi > 1`, LAMMPS
sets `forward_comm_legacy = exchange_comm_legacy = reverse_comm_legacy =
forward_pair_comm_legacy = ... = 1`. `src/KOKKOS/comm_kokkos.cpp:136-157` — the
legacy branch does `atomKK->sync(Host, X_MASK)` and then calls
**`CommBrick::forward_comm`**: the same `nswap` loop, over the same swap table,
posting the same `MPI_DOUBLE` counts, from a host pointer instead of
`k_buf_send.view<DeviceType>().data()`.

So the prediction, made before reading the capture: **the message count and the
byte total must be identical, and the wall clock must not be.**

### What the capture shows

`ucx_tag_send`, per rank, both configurations:

| rank | sends (on) | bytes (on) | sends (off) | bytes (off) |
|---|---|---|---|---|
| 0 | 915 | 51 242 583 | 915 | 51 242 583 |
| 1 | 772 | 51 067 694 | 772 | 51 067 694 |
| 2 | 768 | 50 967 382 | 768 | 50 967 382 |
| 3 | 762 | 51 219 894 | 762 | 51 219 894 |
| 4 | 849 | 51 269 754 | 849 | 51 269 754 |
| 5 | 754 | 51 120 086 | 754 | 51 120 086 |
| 6 | 753 | 51 067 378 | 753 | 51 067 378 |
| 7 | 752 | 50 968 082 | 752 | 50 968 082 |

Byte-for-byte identical on all eight ranks. Halo-sized (≥ 4096 B) sends: **621
per rank in both**. The `ib` side, from the two `-p ib`-only runs (no ucx
provider in the process at all):

| | ib-on | ib-off |
|---|---|---|
| `ib_post_send` | *absent* | *absent* |
| `ib_poll_cq` | *absent* | *absent* |
| `ib_post_recv` | 10 240 | 10 240 |
| `ib_queue_pair` | 544 | 544 |
| `ib_modify_qp` | 560 | 560 |
| `ib_completion_queue` | 928 | 928 |
| `ib_mem_region` | 775 | 727 |
| `mem_object` | 551 | 503 |
| `comm_edges` | 0 | 0 |

Bare wall clock, same allocation, no tracer:
`Loop time of 0.0356148 on 8 procs` (on) vs `0.0722312` (off) → **2.03×**.

### Which reading the numbers support

The brief warns that a knob moving traffic to an unhooked door shows up as
*less* traffic and is a tool finding, not a speedup. That is testable here and
the test comes out negative:

- **Not tool blindness.** Blindness would require the `gpu/aware on` capture to
  show *fewer* messages than `off`. It shows the *same* messages — identical
  counts and identical byte totals — and it does so independently on two
  providers with different hook sets. A door that both `ib` and `ucx` missed in
  exactly the amount that made two runs agree to the byte is not a plausible
  reading.
- **Not a message-layer performance result either.** The 2.03× is real but it is
  not fewer or larger messages; it is the per-step `atomKK->sync(Host, X_MASK)`
  device→host copy that the legacy path adds. That copy is a `cudaMemcpy`, not a
  message, so it is invisible to `ib` and `ucx` **by construction** — neither
  provider hooks it, and neither claims to. Seeing it needs `-p gpumem` or a
  kernel/copy provider, and that is a scope statement about this capture, not a
  defect.
- **The capture does carry the difference's fingerprint**, in the only place it
  should: `ib_mem_region` 775 → 727 and `mem_object` 551 → 503. Different
  buffers get registered when the staging buffer is host memory. Nothing else
  moved.

So the honest headline is the null one, and the null result is the finding: the
knob is a *memory placement* switch, not a *communication* switch, and a trace of
the communication layer is the instrument that proves it rather than assuming it.

One caveat, stated so nobody reads a difference into it: the two runs' *top-12
message-size histograms* print different rows. That is `ORDER BY n DESC LIMIT 12`
over ~30 sizes tied at n = 19 with no deterministic tiebreak in the query — the
totals and the counts above are the comparison, not the truncated histogram.

---

## C. The capture is right — claim (1)

LAMMPS's own log, independent oracle: `2 by 2 by 2 MPI processor grid`.

| assertion | result | what it derives, and from where |
|---|---|---|
| `halo_endpoint_fanout` | **ok** | `comm_brick.cpp:351` `nswap = 2*(maxneed[0]+maxneed[1]+maxneed[2])`; for this box `maxneed[d] = int(2.8*2/67.19)+1 = 1`, so 6 swaps; `procgrid[d]=2` makes `MPI_Cart_shift` return the same rank both ways, so **3** distinct peers. Capture: exactly 3 endpoints carry halo traffic on every rank. |
| `heavy_sends_uniform` | **ok** | `nswap` is global (`comm_brick.cpp:342-351` Allreduces `recvneed`), so the halo message *count* is identical on every rank though the *sizes* are not. Capture: **621 on all 8 ranks**, both configurations. |
| `halo_send_quantum` | **ok** | every `MPI_Send` in `comm_brick.cpp`/`comm_kokkos.cpp` passes `MPI_DOUBLE`, and pml_ucx hands UCX a contiguous byte count, so every halo message is a multiple of 8 B. |
| `reneighbour_period_visible` | **ok** | `neigh_modify delay 0 every 20 check no` freezes the ghost list for 19 of every 20 steps, so a swap's byte count repeats 19 times in a row. Capture: run length **19** carries 4389 of 4968 halo messages (next: run length 1, 387). |
| `ranks_participate` | **ok** | all 8 ranks present, none extra. |
| `halo_peer_fanout` | **xfail** | same claim as row 1 in *rank* space; blocked by finding D1. |
| `peer_rank_resolvable` | **xfail** | finding D1 itself, as an assertion. |
| `halo_peers_grid_adjacent`, `halo_symmetric`, `no_self_halo` | ok, **vacuously** | negative checks over the peer-resolved edge relation, which is empty. See the note below. |

Two numbers worth pulling out because they are exact, not approximate:

- **621 = 3 × 207.** Rank 0's three halo endpoints carry 207 heavy sends each
  (19 816 936 / 16 966 296 / 14 447 776 B). The fan-out derived from
  `comm_brick.cpp` is in the capture, per-endpoint, exactly.
- **240 size-runs.** Grouping halo sends by `(rank, endpoint, size)` gives 231
  runs of length 19, 3 of length 38 and 1 of length 57 — i.e. 231 + 3×2 + 1×3 =
  **240 = 8 ranks × 6 swaps × 5 rebuild intervals**, with the 38s and 57 being
  adjacent intervals that happened to produce an identical byte count and so
  merged into one group. `steps / reneighbour_every` and `nswap` both come back
  out of the wire.

The per-endpoint byte totals differ by 37 % (19.82 / 16.97 / 14.45 MB) and that
is **not** a defect: LAMMPS's six-swap scheme forwards ghosts acquired in earlier
dimensions, so the later swaps carry strictly larger slabs
(`comm_brick.cpp:383-396` and the `slablo/slabhi` construction above it). A
symmetric-looking box does not imply symmetric halo volumes.

**On the three vacuous passes.** A negative check over an empty relation reports
no offending rows and asserts nothing. They are deliberately *not* marked
`expect_fail` — they do not fail — and `peer_rank_resolvable` is shipped beside
them as the guard that makes their vacuity visible instead of silent. When the
missing hook lands they become real checks with no edit to any of them.

---

## D. What pcomm could not see — claim (4), named to symbols

### D1. `ucp_worker_query` is not hooked, and it is the door MPI actually uses

pcomm hooks `ucp_worker_get_address` — `providers/ucx/ucx_hooks.hpp:181`,
exported at `providers/ucx/ucx.map:17` — and that is the only producer of
`UcxWorkerAddress`. But OpenMPI 4.1.5's **two-sided** path does not call it:

```
$ nm -D --undefined-only mca_pml_ucx.so | grep ucp_worker_
    ucp_worker_create   ucp_worker_destroy   ucp_worker_progress
    ucp_worker_query    ucp_worker_release_address
$ nm -D --undefined-only mca_osc_ucx.so | grep ucp_worker_get_address
    ucp_worker_get_address
```

`ompi/mca/pml/ucx/pml_ucx.c:95-99` is the reason:

```c
attrs.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS |
                   UCP_WORKER_ATTR_FIELD_ADDRESS_FLAGS;
status = ucp_worker_query(ompi_pml_ucx.ucp_worker, &attrs);
```

(`pml_ucx.c:131`'s `ucp_worker_get_address` is the `#else` branch and is compiled
out — the `.so` does not import it.) Only `mca_osc_ucx` and `mca_spml_ucx` use
the old door.

**Consequence, measured.** 16 `ucx_worker` rows, **8** `ucx_worker_address` rows
— one per rank, and it is the *one-sided* worker, which carries no application
traffic. The join that `providers/ucx/ucx_records.hpp:275-294` describes as "the
only thing in the UCX records that crosses a rank boundary" therefore resolves
nothing for the traffic that matters:

```
rank  sends  with_ep_connection  with_peer_rank
0      915          915               148
1      772          772                 8
2      768          768                 8
...
7      752          752                 8
```

and restricted to halo-sized messages, **with_peer_rank = 0 on every rank, for
all 621**. Rank 0's endpoint 230 carries 207 halo sends / 19 816 936 B and a
`remote_prefix_hash` that appears in no `ucx_worker_address` row in the capture.

The endpoints are all there; every send joins its `ucx_ep_connection`. What is
missing is the far side of the hash, and it is missing because of one unhooked
symbol.

**Shape of the fix** (not applied — findings, not fixes): one more
`DEFINE_PRELOAD_HOOK` for `ucp_worker_query`, emitting `UcxWorkerAddress` when
`UCP_WORKER_ATTR_FIELD_ADDRESS` comes back in `attr.field_mask`. The record, the
hash scheme and the join all already exist. Five of this case's assertions flip
from vacuous/xfail to real on that one hook.

### D2. `ibv_post_send` is not UCX's send door — now confirmed by an application

`ib_post_send` and `ib_poll_cq` are **absent from all four exports** — both
providers, both knob settings, across two nodes — while the control plane and
every posted receive are fully captured:

```
ib_post_recv 10240   ib_queue_pair 544   ib_modify_qp 560
ib_mem_region 775    ib_completion_queue 928
ib_post_send   —     ib_poll_cq      —
```

10 240 = 8 ranks × 1280 receives, and zero sends. The asymmetry is the
mechanism: UCX's mlx5 transports post receives through the verbs API but build
send WQEs themselves and ring the doorbell, so the send side never reaches
`ctx->ops.post_send`. This is `PLAN-20260815-1540` Stage 3's measurement
(job 44635003) reproduced by third-party software over a real fabric rather than
by a microbenchmark — which is what that plan's "blocked on research" was
missing. `-p mlx5dv` is the obvious next probe and was not run here.

It is not a coexistence artefact: the `-p ib`-only runs, with no ucx provider in
the process, give the identical table.

### D3. Therefore `comm_edges` is empty for an MPI application

`comm_edges` returned **0 rows** in all four captures. By construction, not by
accident: `pcomm-read/link_sql.hpp:1112` builds it from `ib_post_send`,
`mlx5_device_send` and `gpu_access` only — there is no UCX lane. So for
MPI-over-UCX all three routes to a rank→rank matrix are closed at once: the ib
lane has no sends (D2), the ucx lane is not wired into the table at all, and the
hand-built UCX join is blocked by D1.

That is the single most consequential thing this study found about the tool. The
traffic *is* captured — 51 MB and ~800 messages per rank, sizes and counts
correct, fan-out correct in endpoint space — and the headline derived table over
it is empty.

---

## Reproduce

```bash
sbatch tests/hpc/apps/02_lammps/job.sbatch        # 2 nodes, ~45 min walltime
```

Six executions in one allocation: bare `on` (carrying the allocation counter),
bare `off`, then `ib,ucx` × `on|off` and `ib` × `on|off` captured, exported to
CSV and Parquet, followed by the comparison queries and the assertion runner.
Question A needs no allocation at all — §A1-A3 are `strings`/`nm` on the login
node, and §A4 runs on `alogin2`, which has four H100s of its own.
