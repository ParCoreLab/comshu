# 01_gromacs — what `GMX_DISABLE_DIRECT_GPU_COMM` looks like in a capture

GROMACS 2025.2 (MN5 module `gromacs/2025.2-gcc-nvhpcx`), benchRIB (2,136,412
atoms, Max Planck benchmark set), 2 nodes x 4 H100, 8 MPI ranks = 7 PP + 1 PME,
Open MPI 4.1.5 over UCX 1.15 (HPC-X 2.16) over NDR InfiniBand. Captured with
`-p ib,ucx`.

---

## Section 0 — PREDICTIONS, written before the first capture existed

Recorded 2026-08-26, after reading the GROMACS source and before the job had
run. Everything below is derived from GROMACS's code and the mdrun command line;
nothing is read off a capture. Later sections report what actually happened and
say explicitly where a prediction was wrong.

### P0 — the knob is not the one the brief (or the module) names

`GMX_ENABLE_DIRECT_GPU_COMM` **does not exist in GROMACS 2025.2.** The only
reference to direct GPU communication in the whole source tree is

```
src/gromacs/taskassignment/decidegpuusage.cpp:794
    const bool isDisabledByUser = getenv("GMX_DISABLE_DIRECT_GPU_COMM") != nullptr;
```

Direct GPU communication is **on by default** in a CUDA + library-MPI build
(`decidegpuusage.cpp:781-785`); the environment variable only turns it *off*.
The `GMX_ENABLE_*` spelling was the GROMACS 2021/2022 opt-in and has been dead
since 2023.

MN5's module file still sets the dead variable:

```
/apps/ACC/modulefiles/applications/gromacs/2025.2-gcc-nvhpcx.lua
    setenv("GMX_ENABLE_DIRECT_GPU_COMM","1")
```

It is a no-op. Anyone who "turned direct GPU comm on" on this machine by setting
it changed nothing, and anyone who assumed the default was *off* had it
backwards. The job script unsets it and drives `GMX_DISABLE_DIRECT_GPU_COMM`.

### P1 — topology, and why it is derivable

`-npme 1` splits 8 ranks into 7 PP + 1 PME. `npp = 7` is prime, so `nx*ny*nz =
7` admits only a permutation of `7x1x1`; `-dd 7 1 1` states it explicitly.

PP -> PME assignment, from `src/gromacs/domdec/domdec.cpp:540` and `:623`:

```
ddindex2pmeindex(i) = (i*npme + npme/2) / npp
pme_of(r)           = npp + ddindex2pmeindex(r)      [pmeRanks.empty() branch]
```

which at `npme = 1` is 7 for every PP rank. Every PME message a PP rank sends is
addressed to that one rank (`ewald/pme_pp.cpp:184,197,212,296`;
`ewald/pme_pp_comm_gpu_impl_gpu.cpp:256`).

Halo: a 1-D periodic DD grid of 7 cells gives cell `x` exactly two neighbours,
`(x+-1) mod 7`. GROMACS stages halo exchange one dimension at a time
(`domdec/gpuhaloexchange_impl_gpu.cpp:489-492`, `domdec/haloexchange.cpp:89,126`),
so with one dimension there are no forwarded diagonal zones and the partner set
is exactly those two.

Predicted graph: **one hub (rank 7) of degree 7, and a 7-cycle among the PP
ranks.** `mpirun --map-by ppr:4:node` puts ranks 0-3 on node A and 4-7 on node B,
so of the 13 undirected edges exactly six cross the node boundary and are the
only ones `ib` can witness:

| edge | kind | crosses nodes |
|---|---|---|
| 3-4, 6-0 | halo | yes |
| 0-7, 1-7, 2-7, 3-7 | PME | yes |
| 0-1, 1-2, 2-3, 4-5, 5-6 | halo | no |
| 4-7, 5-7, 6-7 | PME | no |

### P2 — volume, from the system size alone

Every PP rank sends its **home** atoms' coordinates to the PME rank once per
force evaluation, and the PP ranks' home sets partition the system, so the
PP->PME coordinate volume per step is `natoms * 12 B` however the decomposition
moved atoms:

```
coordinates : 12 B * 2,136,412 * 1001 steps    = 25,662,580,944 B
charges     :  4 B * 2,136,412 *   11 rebuilds =     94,002,128 B   (nsteps/nstlist + 1)
                                         total = 25,756,583,072 B  ~= 25.76 GB
```

`natoms` is offset 72 of `benchRIB.tpr` (big-endian uint32 = 2136412), read from
the input file, not from any log or capture.

### P3 — what the knob changes, and what it does not

The prediction the whole exercise turns on, and it is deliberately
counter-intuitive:

**Message counts and byte counts are predicted to be IDENTICAL in both arms.**

On a *library-MPI* build (which this is — `libmpi.so.40` from HPC-X 2.16),
direct GPU communication does not bypass MPI. It hands MPI a **device pointer**
instead of a host one:

| | direct ON | direct OFF |
|---|---|---|
| halo | `MPI_Irecv(recvPtr,...)` / `MPI_Send(sendPtr,...)`, `Float3*` in **device** memory — `gpuhaloexchange_impl_gpu.cpp:489,492` | `copyFromDeviceBuffer` -> `h_outOfPlaceSendBuffer_` -> the same MPI calls on **host** memory (`:517-525`), or the CPU halo path (`haloexchange.cpp:89,126`) |
| PME-PP | `MPI_Isend(sendptr_x, sendSize*DIM, MPI_FLOAT, pmeRank_)` on a **device** buffer — `pme_pp_comm_gpu_impl_gpu.cpp:256` | `MPI_Isend(x.data(), n*sizeof(rvec), MPI_BYTE, dd->pme_nodeid)` on **host** `x` — `pme_pp.cpp:296` |

Same number of calls, same element counts, same peers. So:

- `ucx_tag_send` row counts and `count` totals: **predicted equal within a few
  percent** between the arms. `pme_coord_volume.sql` asserts the same expected
  value in both, which is the point — one specification, two runs.
- The discriminator is **which memory the NIC was asked to register.**
  `mem_object.device` is filled by `pcomm::core::device_of()` at every ibverbs
  registration door (`ibverbs_provider.cpp:580, :622, :914`) and is 65535 for
  host memory. Predicted: **direct ON has at least one NIC-registered range with
  a real device ordinal; direct OFF has none.** That is
  `gpudirect_registration.sql`.
- Side effect, not the main claim: with direct comm off, GPU update under domain
  decomposition is unsupported, so `-update auto` falls back to CPU (which is
  exactly why the job script does not pass `-update gpu` — that would be a fatal
  error in the OFF arm). The bare runs should show it in `Update task` and ns/day.

### P4 — the trap, stated in advance

If the ON capture shows **less** traffic than the OFF capture, the correct
reading is *not* "the knob saved bytes". It would mean the traffic left through a
door pcomm does not hook. The specific candidate is ruled out by the build: the
`cudaMemcpyPeerAsync` peer-to-peer path
(`gpuhaloexchange_impl_gpu.cpp: communicateHaloDataPeerToPeer`) is
`supportedThreadMpiBuild` only, and this is a lib-MPI build, so it cannot fire.
Intra-node device transfers therefore still go through `ucp_tag_send_nbx`, which
is hooked. **Prediction: no traffic disappears.**

### P5 — the gap expected to be found (claim 4)

`ucx_tag_send` records `buffer` (an interned address), `count`, `tag`,
`endpoint` — and **no memory type and no device.** `ucp_request_param_t` has a
`memory_type` field, but Open MPI's `pml_ucx` does not set it (UCX sniffs the
pointer itself), so the API argument genuinely does not carry it. But
`pcomm::core::device_of()` already exists, is header-only, and is applied by the
ibverbs provider to every address it registers — and the UCX provider never asks
it about the send/recv buffer.

Predicted consequence: at the UCX layer — the layer that carries GROMACS's
*intent* — the capture cannot tell a host-staged halo exchange from a GPU-direct
one. The evidence has to come from the verbs layer underneath, which sees only
six of the thirteen edges. Roughly half the traffic in this run is intra-node
and, for this question, unattributable.

---

## Section 1 — RESULT

Job **45082564**, 2 nodes x 4 H100, `acc_debug`, 4 min 13 s wall for all four
mdrun invocations. Every prediction in Section 0 held except **P3's
discriminator**, which failed in a way that is itself the most useful thing in
this report.

### 1.1 The knob fired, and it is worth 19 %

GROMACS says so itself, in the ON arm only (`bare-on.log:342`,
`trace-on.log:342`, absent from both OFF logs):

```
GPU-aware MPI detected, enabling direct GPU communication.
```

| arm | bare ns/day | traced ns/day | tracer cost |
|---|---|---|---|
| direct GPU comm **ON** | 60.611 | 60.190 | 0.69 % |
| direct GPU comm **OFF** | 50.760 | 50.189 | 1.12 % |
| **speedup from the knob** | **+19.41 %** | **+19.93 %** | |

Two things worth stating. First, `pcomm run -p ib,ucx` costs about **1 %** on a
2.1 M-atom production MD run — the tracer is cheap enough that the performance
claim survives being measured under it. Second, GPU update was refused in
**both** arms for reasons that have nothing to do with the knob (`trace-on.log:335`:
virtual sites, triangle constraints, coupled-constraint count), so the 19 % is
attributable to halo exchange and PME-PP communication alone and not to a
different integrator landing on a different device.

### 1.2 The knob moved essentially no bytes and no messages — as predicted

This is claim (3), and the answer is the counter-intuitive one:

| | direct ON | direct OFF | delta |
|---|---:|---:|---:|
| `ucx_tag_send` rows | 103,292 | 103,215 | **+77 (+0.075 %)** |
| `ucx_tag_send` bytes | 72,548,421,404 | 72,547,832,735 | **+588,669 (+0.0008 %)** |
| PME rank outbound bytes | 25,662,688,388 | 25,662,688,388 | **0 (byte-identical)** |
| `ucx_worker_progress` rows | 618,754 | 307,757 | +310,997 (+101 %) |
| `ib_mem_region` rows | 1,564 | 1,625 | -61 |
| `ib_mem_region` bytes | 8,784,629,024 | 6,985,567,776 | **+1,799,061,248 (+25.8 %)** |
| `mem_object` with a device | **0** | **0** | 0 |
| `ib_post_send` rows | **0** | **0** | 0 |
| raw capture | 55 MB | 39 MB | +16 MB |

72.5 GB of tagged sends either way, agreeing to eight parts in a million. The
PME rank's outbound total is **identical to the byte** across a knob that
changes the buffer it reads from. That is the trace saying, in its own units,
what `pme_pp.cpp:296` and `pme_pp_comm_gpu_impl_gpu.cpp:256` say in source: same
calls, same counts, same peers, different memory.

**The 16 MB capture-size difference is not traffic.** It is 310,997 extra
`ucx_worker_progress` rows and nothing else — every other table is within 61
rows. `ucx_worker_progress` is documented in `ucx_records.hpp` as Full-mode
telemetry that "varied by a factor of two between two runs of the identical
workload", and it is a count of progress calls that *found work*, not of bytes.
Reading the bigger capture as more communication would be exactly backwards.

### 1.3 The number the capture gets right, independently

`asserts/pme_rank_force_volume.sql`, passing in **both** arms:

```
predicted  rvec_bytes * natoms * (nsteps+1) = 12 * 2,136,412 * 1001 = 25,662,580,944 B
observed   sum(ucx_tag_send.count) over ranks >= npp                = 25,662,688,388 B
excess                                                              =       107,444 B  (+0.00042 %)
```

The oracle is the tpr header (`natoms` at byte offset 72) and the mdrun command
line (`-nsteps`, `-npme`). The excess is 107 B per step, which is the
energy/virial struct `gmx_pme_send_force_vir_ener` sends alongside the forces.
**Four parts per million, against a number the tool never saw.**

A second, independent volume check from GROMACS's own DD statistics
(`bare-on.log:590-592`) — halo `2 x 708,354.5` atoms/step, vsites `3 x 5,675.7`,
LINCS `3 x 79,698.5`:

```
(2*708354.5 + 3*5675.7 + 3*79698.5) * 12 B * 1000 steps  = 20.07 GB  halo+vsite+LINCS
                                        PP -> PME coords = 25.66 GB
                                                predicted = 45.74 GB
observed sum of ucx_tag_send.count over the 7 PP ranks    = 46.89 GB   (+2.5 %)
```

2.5 % over, which is the DD redistribution at the 11 pair-list rebuilds plus the
global reductions — neither of which appears in that log line. Both oracles land.

### 1.4 The discriminator did NOT work, and it is not the reading you would guess

`gpudirect_registration.sql` **fails in the ON arm and passes in the OFF arm**:
all 1,484 NIC-registered ranges in the direct-GPU-comm run resolve to host
memory (`mem_object.device = 65535`), and there are zero `ibv_reg_dmabuf_mr`
rows in either arm.

So the run that GROMACS says used GPU-direct communication, and that is 19 %
faster for it, shows the NIC touching **no device memory at all**.

There is a real signal next to it, though, and it points the same way the source
does: with direct comm ON, UCX registers **1.80 GB more host memory in 61 fewer
regions**. Bigger registrations, more of them by volume, fewer by count.

Three readings, and Section 3 says which one the evidence supports.

### 1.5 The probe that settled it — job 45088478

One extra 25-minute job, ON arm only, 200 steps, `-p ib,ucx,mlx5dv`, to separate
the two readings of 1.4 (see `probe.sbatch` for the statement of the two
hypotheses, written before it ran).

| | main job `-p ib,ucx` | probe `-p ib,ucx,mlx5dv` |
|---|---:|---:|
| `mlx5_umem` | — (provider absent) | **2,032** |
| `mlx5_devx_qp` / `_connect` | — | 1,328 / 1,328 |
| `mlx5_uar` | — | 304 |
| `mlx5_devx_cmd` | — | 6,192 |
| `mem_object` | 1,484 | **3,018** |
| `mem_object` with a device | 0 | **0** |
| `mlx5_umem` with a device | — | **0** (all `device=65535`, `dmabuf_fd=-1`, `binding=by_addr`) |
| `mlx5_device_send` | — | **0** |
| `ib_post_send` | 0 | **0** |
| `ucx_tag_send` | 103,292 | 24,452 |

The mlx5dv provider finds **1,534 registrations the ibverbs provider never saw**
— so there really was a second registration door in play, and pcomm does hook
it. Every one of those registrations is still **host** memory.

That kills the hypothesis that device memory was being registered through a door
we had not loaded. Combined with `UCX_IB_GPU_DIRECT_RDMA=try` (try, not force —
`ucx_info -c` from a compute node) the conclusion is:

> **UCX did not use GPUDirect RDMA for GROMACS's device buffers. It pipelined
> them through host memory, so the NIC genuinely only ever saw host pages, and
> pcomm's capture of that fact is CORRECT.**

The affirmative evidence for it — not merely the absence of device rows — is in
1.2: with direct GPU comm ON, UCX registers **1.80 GB more host memory**
(+25.8 %) than with it off. If the NIC were reading the GPU directly, the arm
whose buffers live on the GPU would need *less* host bounce space, not a quarter
more. More host registration in the device-buffer arm is the signature of a
host-staged rendezvous pipeline. The 19 % that GROMACS wins is therefore won on
the *GROMACS* side of the boundary — it stops doing its own D2H/H2D staging and
lets UCX overlap the copy with the transfer — not by taking the NIC to the GPU.

## Section 2 — assertions

Ten assertions ran per arm (`asserts.sh`, not `suite/sql_check.sh` — see
NOTES.md), plus thirteen cross-cutting ones that skip for want of tables.

| assertion | direct_ON | direct_OFF | what it means |
|---|---|---|---|
| `pme_fanin` | ok | ok | every PP rank has an edge to the PME rank `pme_of(r)` derives |
| `pme_returns_forces` | ok | ok | and the PME rank has one back to every PP rank it serves |
| `pme_rank_force_volume` | **ok** | **ok** | 25,662,688,388 B vs 25,662,580,944 B predicted — 4 ppm |
| `ranks_complete` | ok | ok | all 8 ranks recorded |
| `registration_has_object` | ok | ok | every MR has a resolvable object |
| `gpudirect_registration` | **XX** | ok | the discriminator, and 1.5 explains it |
| `pme_coord_volume` | xfail | xfail | needs the broken cross-rank join |
| `halo_partners` | xfail | xfail | needs the broken cross-rank join — **RETIRED 2026-09-02, see below** |
| `entity_close_accounted` | xfail | xfail | pre-existing, not this case |
| `mem_owner_unambiguous` | xfail | xfail | UCX rcache re-registers one base at growing lengths |
| `verbs_is_internode_only` | skip | skip | `comm_edges` is empty — see F1 |
| `expect_tables` | **`ib_post_send` empty** | same | F1 |

> **Two corrections to the table above, 2026-09-02.** `halo_partners` and
> `pme_coord_volume` are listed as `xfail`, and neither was ever in
> `meta.json`'s `expect_fail_asserts` — so both reported as ordinary failures to
> anyone who ran them, and this table said otherwise. The row is the claim that
> was wrong, not the run.
>
> `halo_partners` has been retired. Its oracle was correct; its query resolved a
> send's peer through `ucx_ep_connection -> ucx_worker_address.prefix_hash`, the
> join this very report measured as resolving the wrong worker on Open MPI
> 4.1.x. It was therefore an assertion that could only pass by accident.
> `asserts-r2/halo_and_pme_peers_match_the_decomposition.sql` makes the same
> claim from the MPI tag with no cross-rank join, and passes: on the round-2
> capture rank 0's top senders are 7 (PME), 1 and 6 — exactly `(0±1) mod 7` plus
> the PME rank — while rank 4, this report's anomaly, contributes 5.7 kB against
> their 70–183 MB. Rank 4 is `0^4`, an Open MPI recursive-doubling collective
> partner, and always was.
>
> `pme_coord_volume` was fixed separately in `71e2e05`: it failed because
> `job-r2.sbatch` fed it round 1's `meta.json` (`nsteps: 1000`) while the run did
> 200 steps. See `NOTES.md`.

**`pme_fanin` and `pme_returns_forces` pass for a weak reason and I do not want
them read as strong.** They ask only whether an edge exists, and the broken join
(F2) still yields a one-byte edge between nearly every pair, so they would pass
on a capture that had lost all the traffic. `pme_rank_force_volume` is the one
that carries weight, because it needs nothing but `rank` and `count`.

None of the five xfail markers existed before the run; each carries its evidence
in `meta.json`. No assertion was edited to match what the tool emitted —
`pme_coord_volume.sql` stands exactly as written and
`pme_rank_force_volume.sql` was added beside it, asking the same oracle of a
part of the capture that works.

### Predictions, scored

| | verdict |
|---|---|
| P0 the knob is `GMX_DISABLE_*` and the module sets a dead variable | **right** |
| P1 DD grid 7x1x1, 1 PME rank | **right** — GROMACS printed exactly that |
| P2 volume = `12 * natoms * (nsteps+1)` | **right to 4 ppm** |
| P3a counts and bytes unchanged by the knob | **right** — +0.075 % / +0.0008 % |
| P3b device memory registered iff direct comm on | **WRONG** — and 1.5 says why |
| P4 no traffic disappears | **right** — 103,292 vs 103,215 messages |
| P5 `ucx_tag_send` cannot tell host from device | **right**, and it mattered more than expected |

## Section 3 — what turned out to be wrong or missing in pcomm

### F1 — On the HPC-X/UCX stack, pcomm records zero transmitted payload. Named to a symbol.

`suite/README.md` and `PLAN-20260815-1540` Stage 3 record this as "`ibv_post_send`
is not UCX's send door on MN5 (measured, job 44635003) and no replacement has
been named." This run names it, and the situation is worse than "not the door":

```
nm -D --undefined-only libucp.so.0     | grep -c ibv_post_send   ->  0
nm -D --undefined-only libuct_ib.so.0  | grep -c ibv_post_send   ->  0
nm -D --undefined-only libuct_ib.so.0  | grep -oE 'ibv_reg_[a-z0-9_]*|mlx5dv_devx_[a-z_]*'
      1 ibv_reg_dmabuf_mr
      1 ibv_reg_mr_iova2
      1 mlx5dv_devx_obj_create
      2 mlx5dv_devx_umem_reg
```

UCX's InfiniBand transport **has no reference to `ibv_post_send` at all.** It
registers its queues with `mlx5dv_devx_umem_reg`, creates them with
`mlx5dv_devx_obj_create`, writes WQEs into that memory itself and rings a
doorbell it mapped itself. There is no ibverbs data-plane door to hook, so
`ib_post_send` is not "missing entry points" the way case 08 was — it is
structurally unreachable here.

The named replacement is the DEVX plane, which pcomm already has a provider for.
But **loading it does not close the gap.** With `-p ib,ucx,mlx5dv` the probe
recorded the entire control plane — 1,328 `mlx5_devx_qp`, 1,328
`mlx5_devx_qp_connect`, 304 `mlx5_uar`, 2,032 `mlx5_umem`, 6,192 `mlx5_devx_cmd`
— and **`mlx5_device_send` stayed empty**, against 24,452 tagged sends and
~17 GB moved. The mlx5dv provider decodes device-initiated sends for NVSHMEM's
IBGDA queues; it does not decode the send queues UCX drives from the host.

Consequences, all observed here:

- `ib_post_send`, `ib_poll_cq`, `mlx5_device_send` all empty.
- `comm_edges` is empty (`comm_edges.parquet` = 196 bytes, header only, in
  **both** arms). The derived traffic layer — `comm_edges`, `wr_flow`,
  `endpoint`, `object_access` — has nothing to work with, so every
  topology-in-rank-space question is unanswerable for an HPC-X application.
- `verbs_is_internode_only` reports `[skip]`, which is honest but means the
  suite's whole verbs vocabulary is silent on the most common MPI stack on this
  machine.

**Everything quantitative in this report comes from the `ucx` provider**, which
records the application's intent. That is a real capability — 72.5 GB of sends
placed to four parts per million — but a reader should know that the wire layer
contributed no traffic at all.

### F2 — `ucp_worker_query` is not hooked, and it breaks the only cross-rank join in the UCX records

`ucx_ep_connection` is documented as "the only thing in the UCX records that
crosses a rank boundary": join `remote_prefix_hash` to another rank's
`ucx_worker_address.prefix_hash`. On this stack it does not work, and the reason
is one unhooked symbol:

```
nm -D --undefined-only mca_pml_ucx.so | grep ucp_worker
                 U ucp_worker_query
```

That is the whole list. Open MPI 4.1.5's `mca_pml_ucx_send_worker_address()`
publishes its address with `ucp_worker_query(worker, {UCP_WORKER_ATTR_FIELD_ADDRESS
| UCP_WORKER_ATTR_FIELD_ADDRESS_FLAGS})` — it never calls
`ucp_worker_get_address`, which is the only address door
`providers/ucx/ucx_hooks.hpp` declares.

Measured consequence, per arm:

```
workers per rank                  2      (pml_ucx's, plus a second)
ucx_worker_address rows per rank  1      the OTHER one
ucx_ep_connection rows          117
      ... that resolve            64
distinct remote hashes seen       24     distinct hashes recorded: 8
```

So the rank-to-rank matrix is built from the wrong worker. 61 of its 64 resolved
edges carry **one byte**; the two that carry bulk (0->4 with 61 MB, 4->0 with
56 KB) are Open MPI wireup on the second worker, not GROMACS. The PP->PME edge
that should carry 25.76 GB carries **7 bytes**, which is why
`pme_coord_volume.sql` is xfail.

Fix: hook `ucp_worker_query` (and `ucp_worker_get_address_v2`) as address doors
alongside `ucp_worker_get_address`, emitting the same `UcxWorkerAddress` row.
`UCP_WORKER_ATTR_FIELD_ADDRESS_FLAGS` with `UCP_WORKER_ADDRESS_FLAG_NET_ONLY`
also means the published address is a *different byte string* from the full one,
which is presumably why 24 distinct remote hashes exist for 8 ranks — a hook on
`ucp_worker_query` would record the address the peer actually dials.

This is a one-hook fix that turns two xfail assertions green and makes rank-level
topology available for every Open MPI 4.1.x application.

> **F2 IS CLOSED — verified 2026-09-02 on the round-2 capture (job 45235734).**
> The hook was added (`providers/ucx/ucx_hooks.hpp` declares `ucp_worker_query`
> and `ucx_provider.cpp` emits `UcxWorkerAddress` from it), but nobody had gone
> back and measured whether the join actually resolves. It does, completely:
>
> | | round 1 | now |
> |---|---|---|
> | `ucx_ep_connection` rows that resolve | 64 of 117 | **117 of 117** |
> | distinct remote hashes seen / recorded | 24 / 8 | **24 / 24** |
> | `comm_edges` ucx lane | empty — 196 B, header only | real rank-space topology |
>
> Every remote hash a connection names is now a hash some worker recorded, which
> is what the "24 versus 8" line above predicted would happen once the address
> the peer actually dials was captured.
>
> And the topology it yields is right. The heaviest `comm_edges` ucx edges are
> ranks 2, 3 and 4 against rank **7** at ~750 MB each — rank 7 is the PME rank
> and the hub every PP rank exchanges with, which is the decomposition. The
> `scope` column added the same week splits them correctly too: 4↔7 intra-node,
> 2↔7 and 3↔7 network.
>
> So the sentence above — "the derived traffic layer has nothing to work with,
> so every topology-in-rank-space question is unanswerable for an HPC-X
> application" — is no longer true, and it is the most consequential thing in
> this report to have gone stale. The verbs-plane half of F1 stands: this is the
> UCX provider answering, not `ib_post_send`.

### F3 — `mem_object.device = 65535` cannot say whether attribution was attempted

The most important negative result here — "the NIC never saw device memory" —
rests on `device = 65535` on 3,018 objects. `core/mem_attr.hpp` states the
problem in its own comment: kUnknownDevice "means 'host memory' *and* 'CUDA is
not present' *and* 'the call failed'. Those three imply completely different
work and look identical in the capture."

It provides `PCOMM_TRACE_HOOKS=1` -> `trace_device_attr()` as the discriminator,
but that is a one-shot **stderr print**, not a column, so it cannot be asserted
on and does not survive into the export. I reached the H1 conclusion in 1.5 by
an indirect argument (host registration went *up* 25.8 %), which is sound but is
inference where a bit would have been measurement.

Suggested: one `attribution` column on `MemObject` — `device` / `host` /
`unavailable` / `failed`. It costs nothing at the registration door, which has
the `cuPointerGetAttribute` return code in hand already and currently throws it
away.

### F4 — `ucx_tag_send` carries no memory type, and here it was the only layer that saw anything

Predicted in P5 and confirmed. `ucx_tag_send` records `buffer` (an interned
address) but no device and no memory type, so the layer that captured 100 % of
this run's traffic cannot distinguish a host-staged halo exchange from a
GPU-direct one. `ucp_request_param_t` does have a `memory_type` field and Open
MPI does not set it — but `pcomm::core::device_of()` is header-only, already
linked into every provider, and already applied by the ibverbs provider to every
address it registers. Applying it to the send/recv buffer would give
`ucx_tag_send` the discriminator this whole exercise needed, at the layer that
actually has the data.

### F5 — the harness cannot express a real application's oracle

`suite/sql_check.sh` builds `params` from a hard-coded fifteen-name column list
and `exit 2`s on anything else. This case's oracle is `npp`, `npme`, `nsteps`,
`nstlist`, `natoms` and not one of them can be written down, so the assertions
would have had to smuggle their oracle in as SQL literals — "a golden wearing a
costume" by the suite's own definition. `asserts.sh` here takes the column list
from `meta.json` instead. That one change belongs upstream.

### F6 — a size difference that is not a data loss

The job's own `du -sh` reported the direct_off parquet tree as **9.5 K** beside
direct_on's 9.5 M, which reads as a derived layer that collapsed on one side.
`du -sb` on the same directory afterwards reports **6,559,602 bytes**, and the
per-file listings are comparable throughout. The tree was complete; GPFS block
accounting had not caught up when the job's own `du` ran.

What *is* empty is `comm_edges.parquet` — 196 bytes, header only — and it is
**identically** empty in both arms, for the reason in F1. Nothing about the
derived layer differs between the two configurations. Had that been read as
"direct GPU comm moved the traffic somewhere pcomm cannot see", it would have
been the exact inversion the brief warns about: the traffic never moved
(+0.0008 % bytes), the wire layer was blind in both arms equally, and the knob's
19 % came from work GROMACS stopped doing on its own side of the NIC.
