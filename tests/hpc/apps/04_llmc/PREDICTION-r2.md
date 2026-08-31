# Round 2 — pre-registered prediction, written before the first r2 capture existed

Written 2026-08-29 on the FreeBSD workstation, committed before `run-r2.sh` was
executed for the first time on du04. Derived from **llm.c's own source**
(`~/llmc` on du04, upstream `f1e2ace` + the CUDA-13 `cudaMemAdvise` patch in this
directory), from the record schema (`libpcomm/schema/pcomm.fbs`) and from the
derived-table column contracts in `apps/BRIEF-BLACKBOX.md`. Nothing here was read
off a capture, r1's included: every count below was re-derived from source in
this round, not copied from `PREDICTION.md`.

Round 1 stopped at the raw NCCL tables. **This round is about the layer that did
not exist then** — `pcomm link`'s derived tables — and about a provider no case
in `tests/hpc/cases/` has ever asserted anything about: `kernel`.
`grep -rn gpu_kernel tests/hpc/cases/*/asserts/*.sql` is empty.

## 0. The derivation, redone

    L  = 6        -e d6                      train_gpt2.cu:523
    C  = 384      -e d6                      train_gpt2.cu:523
    Vp = 50304    padded vocab               train_gpt2.cu:583
    T  = 1024     -t 1024 (= max_seq_len)
    S  = 4        -x 4    (steps 0..3 train; step 4 validates and breaks, :1819)
    P  = 4        du04, one node, four A100

    G = L*(12*C^2 + 13*C) + Vp*C + T*C + 2*C
      = 6*1774464 + 19316736 + 393216 + 768
      = 30 357 504 gradient elements per optimizer step per rank

Call sites, all of them, per rank:

| # per step | site | file:line | group? | stream | dtype |
|---|---|---|---|---|---|
| 76 | gradient reduction (12/layer x 6, +4) | `zero.cuh:533` (z0) / `:543` (z1) | inside | nccl_stream | bf16 |
| 76 (z1 only) | optimizer-state gather | `train_gpt2.cu:1113` | inside | nccl_stream | bf16 |
| 1 | final loss | `train_gpt2.cu:958` | **outside** | main_stream | f32 |
| 1 (z1 only) | gradient norm | `train_gpt2.cu:1022` | **outside** | main_stream | f32 |

Outside the loop, both stages: 1 x `multi_gpu_cpu_float_sum` at
`train_gpt2.cu:412` + 2 validations (step 0 and the last step, `-v 1000 > S`,
`:1727`) = **3** f32 all-reduces, all ungrouped, on nccl_stream.

Groups (`ncclGroupStart`/`End` pairs) per step: `L+1 = 7` from backward
(`zero.cuh:530,551`, taken at both stages) plus, at z1 only,
`NUM_PARAMETER_TENSORS = 16` from `gpt2_update` (`train_gpt2.cu:1110-1118`) = 23.

**Every bf16 collective is inside a group; every f32 collective is outside one.**
That is not an observation, it is where the four f32 call sites sit in the source.

## 1. The raw NCCL numbers (re-derived; r1 measured these, r2 must reproduce them
on a *different build* of pcomm)

Per rank, 4 optimizer steps, P=4:

| | `-z 0` | `-z 1` |
|---|---|---|
| `nccl_collective` rows | **311** | **619** |
| bf16 `all_reduce` rows / Σcount | 304 / **121 430 016** | 0 / 0 |
| bf16 `reduce_scatter` rows / Σcount | 0 / 0 | 304 / **30 357 504** |
| bf16 `all_gather` rows / Σcount | 0 / 0 | 304 / **30 357 504** |
| f32 `all_reduce` rows | **7** | **11** |
| every f32 `count` | **4** (`sizeof(float)`, a defect in llm.c) | **4** |
| `nccl_group` rows | **56** | **184** |
| `nccl_p2p` rows | 0 | 0 |
| `nccl_comm` rows | 1/rank, `nranks`=4, one shared non-zero `comm_hash` | same |
| distinct `stream` handles in `nccl_collective` | **2** | **2** |

## 2. The kernel layer — new this round, never asserted by anything

`-p kernel` (CUPTI) records `gpu_kernel` (type 530): one row per kernel
**execution** window, with `name`, `device`, `stream`, `correlation`.

**The claim, and it is a cross-layer one:** NCCL launches **one device kernel per
group**, and one per collective that is not in a group. llm.c states the first
half itself at `zero.cuh:530` — *"NCCL group: aggregate all pointers in a single
NCCL GPU kernel"* — and `pcomm.fbs:906` states it from the other side: *"Group
boundaries: N grouped sends are ONE launch, and nothing in a send row says so."*
Two independent sources, neither of them a capture. Therefore, per rank:

    NCCL device-kernel launches = (nccl_group rows)/2 + (ungrouped collectives)
                                = (nccl_group rows)/2 + (f32 collectives)

| | `-z 0` | `-z 1` |
|---|---|---|
| grouped launches | 4 x 7 = 28 | 4 x 23 = 92 |
| ungrouped launches (the f32 scalar reductions) | 7 | 11 |
| **kernels named `ncclDevKernel%` per rank** | **35** | **103** |
| distinct streams carrying them | **2** | **2** |
| `gpu_kernel.device` per rank | equals that rank's `nccl_comm.cuda_dev` | same |

**Named failure modes.** If the measured number is *lower*, the kernel provider
dropped launches or `nccl_group` does not bracket what it claims to. If it is
*higher*, NCCL's planner split a fused group into several kernels — that is a
fact about NCCL 2.28, not a pcomm defect, and I will report the ratio rather
than adjust the claim. I am predicting equality; I flag the split as the one
non-defect way it can fail, in advance, so that the distinction is not made
after the fact.

Total kernel rows per rank (all kernels, not just NCCL) is **not** predicted:
llm.c's kernel count per step is not derivable from its command line. I predict
only that it is > 1000 for four steps, that every row names a kernel
(`name` non-empty), and that every `device` is in 0..3.

## 3. The derived layer — the point of this round

`pcomm export --parquet` then `pcomm link`. Column contracts from
`BRIEF-BLACKBOX.md`. Predictions, per capture:

| table | predicted rows | why |
|---|---|---|
| `link_health` | **0** | the brief says it must be empty; any row is a defect in the link |
| `rank_identity` | **4** | one per rank |
| `comm_edges` | **0** | see below |
| `transport_mix` | **0** | no `ucx_deep`, no `ib` in the selection |
| `endpoint` | **0** | no verbs QPs exist |
| `object_access` | **0** | no `gpumem` pass, so no `gpu_access` rows |
| `tag_flow`, `tag_recv_resolved` | **0** | UCX-only tables; llm.c has no UCX |

`rank_identity(rank, node_ix, node_hash, device, devices, label)`: 4 rows, one
`node_hash` shared by all four (one node), one `node_ix`, and the four `device`
values a permutation of {0,1,2,3} — because `multi_gpu_config_init` calls
`cudaSetDevice(local_device_idx)` (`zero.cuh:448`) with the node-local MPI rank,
and `mpirun -np 4` on one node hands out local ranks 0..3. This is the first
independent check that the `device` column means the device the application
actually chose. I do **not** predict what `devices` and `label` hold: the
contract names the columns and says nothing about their values, which is itself
a finding for QUESTIONS.md.

**`comm_edges` will be empty, and that is the interesting prediction.** llm.c
moves 4G x 2 B = 242.9 MB of gradient per rank in this run and calls
`ncclSend`/`ncclRecv` zero times (`grep -c 'ncclSend\|ncclRecv' train_gpt2.cu
llmc/*.cuh` = 0). `pcomm.fbs:904` says `NcclP2p` is *"the only NCCL call that
names a peer, and so the only one an exact edge follows from"*. So the rank x
rank traffic table for a workload whose entire communication is collective must
come out empty. Three outcomes, three different findings:

1. **Empty** — a named blind spot (claim 4 of `apps/README.md`): the collective
   layer has no path into `comm_edges`, so the derived table that the suite
   treats as *the* topology view is blank for the most common shape of GPU
   traffic there is. I will state what door is missing and what it would take.
2. **Non-empty with exact peers** — pcomm expanded a collective into an edge set
   it cannot know (NCCL chooses ring/tree, channels, and chunking), i.e. it is
   inventing topology. That would be claim 2, a bug, and I would say so.
3. **Non-empty with a documented approximation** (e.g. an all-to-all fan-out) —
   then the contract line `comm_edges(src_rank,dst_rank,transport,unit,sends,
   bytes)` does not say so anywhere a user can read, which is a documentation
   defect I would file rather than a capture defect.

## 4. The knobs

**Knob A, `-z 0` -> `-z 1`** (r1's knob, re-run because the kernel layer is new):
the collective *algebra* changes as in §1, and now the *execution* changes too —
35 NCCL kernels become 103, a 2.94x rise in launches, against a halving of
logical volume (`2G/P = G/2` at P=4). Round 1 could see the first number and not
the second.

**Knob B, `NCCL_PROTO=Simple NCCL_ALGO=Ring` vs NCCL's default choice**, same
`-z 0` binary and command line. Prediction: **every number in §1 is byte-identical**
— the application's calls cannot know what protocol NCCL picked — while the
`gpu_kernel.name` strings change, because NCCL's device kernels are named after
the algorithm and protocol they implement (`ncclDevKernel_AllReduce_Sum_bf16_RING_LL`
vs `..._RING_SIMPLE`). The launch count stays **35**. This is a difference that
is invisible to the `nccl` provider alone and invisible to the `kernel` provider
alone, and it is the round-2 answer to claim 3.

**Knob C, `-p nccl` alone vs `-p nccl,kernel`** on the same configuration: every
NCCL number identical. If they differ, the kernel provider perturbs the NCCL
capture. I also predict **one pass, not two**: both are hook providers, so
`pcomm run` should not replay the workload (llm.c's banner and losses appear
once in stdout, and there is one set of `rank-*` directories). If it replays,
the cross-layer join in §2 spans two executions and I will say so before using it.

## 5. Guesses that are NOT assertions

Recorded so that being wrong costs nothing, as r1 did with its IB rank asymmetry:

- I expect z1's total NCCL device time per rank to be **lower** than z0's despite
  2.94x the launches, because 30 MB less per step at NVLink bandwidth should beat
  ~68 extra launches at a few microseconds each. I have no oracle for this and no
  assertion depends on it.
- I expect `pcomm read` to show a `clock_anchor` stream once `kernel` is
  selected, because device and host timestamps are in different domains.
- I expect `nccl_collective.stream` (an interned handle) and `gpu_kernel.stream`
  (a raw `CUstream`) **not** to be joinable in the export, so "which kernel
  executed this collective" is not answerable even with both providers loaded.
  If that holds it is a finding, not a defect.
