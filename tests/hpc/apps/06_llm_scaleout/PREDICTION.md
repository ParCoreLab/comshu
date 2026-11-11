# 06_llm_scaleout — pre-registered prediction

Written 2026-09-02 on the FreeBSD workstation, **committed before `job.sbatch`
had ever been submitted and before any capture in this directory existed.**

Application: [karpathy/llm.c](https://github.com/karpathy/llm.c) `train_gpt2.cu`,
upstream `f1e2ace`, already built on MN5 at `$HOME/llmc/train_gpt2cu` by the
04_llmc study. Machine: MareNostrum 5 `acc_debug`, **4 nodes x 4 H100 = 16 GPUs**,
NCCL over NVLink inside a node and NDR InfiniBand between them.

Everything below is derived from llm.c's source, from its command line, and from
the derived-table contracts in `apps/BRIEF-BLACKBOX.md`. Nothing is read off a
capture. Black-box: no file under `providers/`, `libcomshu/` (bar `schema/`),
`comshu-read/`, `comshu-ui/`, `gpu-backends/`, `tools/`, `plans/` or
`BUGS-FOUND.md` was read.

---

## 0. Why this workload, and why 16 GPUs

`tests/hpc/apps/04_llmc/` has run llm.c twice: round 1 at P=4 (du04) and P=8
(MN5, 2 nodes, `-p nccl,ib`), round 2 at P=4 single-node with `-p nccl,kernel`
and the derived layer. Two things follow.

**Why llm.c and not a new workload.** Its collective algebra is already an
*independently validated oracle*: `nccl_collective` row counts, element totals,
datatypes and group boundaries were derived from `zero.cuh` and `train_gpt2.cu`
before either previous run, and matched exactly at two different rank counts on
two GPU generations. Bringing a new data-parallel application would mean
re-earning that oracle from scratch and would put the study's weight on whether
I read the new source correctly. Here, if the collective algebra comes out
right at P=16 as well, that is a control that says the capture is sound before
anything harder is asked of it — and the *P-dependent* half of it (§2.2) is a
genuinely new number, not a repeat.

**Why 4 nodes and not 2.** Two nodes have one node boundary; every cross-node
pair is the same kind of pair, and "hierarchical" is not yet a shape. Four nodes
is the smallest configuration in which the inter-node topology has structure
NCCL must choose — which nodes talk to which — and it is the smallest one where
the question the whole project has never asked can be asked at all:

> **The `nccl` provider says a communicator of 16 ranks executed a collective.
> The `ib` provider says which queue pairs moved which bytes. Do the two views
> agree, and does the derived layer put the intra-node half of a hierarchical
> all-reduce on the correct side of `comm_edges.scope`?**

Round 1 pointed `-p nccl,ib` at two nodes and read `ib_post_send` **raw**. It
never ran `comshu link` on that capture, so `comm_edges`, `endpoint`,
`rank_identity` and `link_health` have never been built for NCCL-over-InfiniBand
at all — and `comm_edges.scope` did not exist when it ran.

---

## 1. The run

```
mpirun -np 16 --map-by ppr:4:node \
   train_gpt2cu -e d6 -b 1 -t 1024 -x 4 -v 1000 -m 1 -s 0 -h 0 -z {0,1} -d 16384
```

* `-e d6` -> `L=6, C=384, Vp=50304`, `-t 1024` -> `T=1024`. gpt2-tiny.
* **`-b 1`, not 04_llmc's `-b 2`, and that is forced.** `llmc/dataloader.h:186`
  asserts `shard_ntok >= num_processes*B*T + 1` on **every** shard, and
  tinyshakespeare's validation split is exactly **32 768** tokens. At P=16,
  `B=2` needs 32 769 and dies in the loader before NCCL initialises; `B=1` needs
  16 385. **Batch size changes no collective in this workload** — the gradient
  tensors are model-shaped, not batch-shaped — which is the same argument
  04_llmc used to move from `-b 4` to `-b 2`, and it is why §2's counts are
  unchanged by it.
* `-d 16384` = `B*T*P`, so `grad_accum_steps = 1` (`train_gpt2.cu:1519`), which
  is what 04_llmc ran at both P=4 and P=8. `train_gpt2.cu:794` gates the
  reduction on `micro_step == grad_accum_steps - 1`, so this keeps one reduction
  per step.
* Rank r lives on node `r / 4` and drives device `r % 4`
  (`cudaSetDevice(local_device_idx)`, `zero.cuh:448`). The bare arm prints
  `hostname` per rank, so this is established by the operating system before any
  capture exists.

**Arms, all in one allocation, bare first.**

| arm | providers | what it is for |
|---|---|---|
| `bare` | none | the workload works. **Also carries `NCCL_DEBUG=INFO`, which is the independent topology oracle for §3.** |
| `z0` | `nccl,ib` | the primary capture |
| `z1` | `nccl,ib` | ZeRO-1; the P-dependent algebra |
| `z0k` | `nccl,ib,kernel` | the third layer: does the launch-count oracle hold at 16 ranks on H100 |
| `z0u` | `nccl,ib,ucx,ucx_deep` | **does `transport_mix` see NCCL at all?** See P4.4 |

---

## 2. The collective layer — what llm.c's source determines

### 2.1 The P-independent numbers (a control, not the finding)

    G = L.(12.C^2 + 13.C) + Vp.C + T.C + 2.C
      = 6.(12.147456 + 4992) + 50304.384 + 1024.384 + 768
      = 30 357 504 gradient elements per optimizer step per rank

llm.c prints `num_parameters: 30357504` itself, which is the check on the
arithmetic. Per rank over 4 steps:

| | `-z 0` | `-z 1` |
|---|---|---|
| `nccl_collective` rows | **311** | **619** |
| bf16 collective rows | 304 `all_reduce` | 304 `reduce_scatter` + 304 `all_gather` |
| f32 `all_reduce` rows | **7** | **11** |
| every f32 `count` | **4** | **4** |
| `nccl_group` rows | **56** | **184** |
| `nccl_p2p` rows | **0** | **0** |
| distinct streams, per rank | **2** | **2** |

76 bf16 collectives per step (12 per layer x 6, plus 4) from the `nelem[]` list
at `train_gpt2.cu:938-945` and `:963`; `L+1 = 7` groups per step from
`zero.cuh:530,551`; at `-z 1` a further `NUM_PARAMETER_TENSORS = 16` groups per
step from `train_gpt2.cu:1110-1118`, giving 23. The f32 rows are the four
ungrouped scalar reductions — `zero.cuh:483`, `:588`, `train_gpt2.cu:958` and
(at `-z 1` only) `:1022` — and every one of them passes `sizeof(float)` where
`ncclAllReduce` wants an element count, so `count = 4`. **That is a defect in
llm.c, the assertion expects 4, and it must not be "fixed".**

`nccl_p2p = 0`: `grep -c 'ncclSend\|ncclRecv' train_gpt2.cu llmc/*.cuh` is zero.
Every byte this application moves is collective.

### 2.2 The P-dependent number — new at P=16

`zero.cuh:572`: `shard_num_parameters = total_parameters / num_processes`.

| | P=4 (measured, 04_llmc) | P=8 (measured) | **P=16 (predicted)** |
|---|---|---|---|
| bf16 sum(`count`), `-z 0` | 121 430 016 | 121 430 016 | **121 430 016** |
| bf16 sum(`count`) per op, `-z 1` | 30 357 504 | 15 178 752 | **7 589 376** |

`4G/P` with `G/16 = 1 897 344`, an integer because `C = 384 = 16 x 24` makes
every one of the 16 parameter tensors divisible by 16. **P3.1: at `-z 1` the
reduce-scatter and all-gather element totals are 7 589 376 each, and equal.**

### 2.3 The kernel layer, if `-p kernel` works here

04_llmc round 2 established, on A100 and P=4, that NCCL launches **one device
kernel per group plus one per ungrouped collective**, so per rank

    launches = nccl_group/2 + f32 collectives = 28+7 = 35   (-z 0)
                                             = 92+11 = 103  (-z 1)

Both numbers are P-independent. **P3.2: 35 NCCL device kernels per rank at
`-z 0`, on H100, at 16 ranks.** Nothing in this project has run CUPTI on MN5;
if the arm fails to load the backend, that is recorded and the rest stands.

---

## 3. The hierarchy — the finding this study exists for

### 3.1 What must be true of the wire

NCCL uses peer-to-peer over NVLink for GPUs inside a node and its `net` (IB)
transport between nodes. Neither fact is comshu's; both are visible in NCCL's own
`NCCL_DEBUG=INFO` output, which prints one line per connection saying
`via P2P/direct pointer` or `via NET/IB/<hca>`. The bare arm collects that, so
the oracle exists before the capture is read.

The `ib` provider hooks the verbs API. NVLink peer-to-peer posts no verbs work
request — it is a device-side store — so:

**P4.1 — every row in `comm_edges` on this capture is `scope = 'network'`.
Zero rows with `scope = 'intra-node'`.** With `-p nccl,ib` the only lane that
can populate `comm_edges` is verbs, and verbs only carries what left the node.

This is a *sharper* claim than it looks, because `comm_edges.scope` is derived
from **node membership**, not from the transport. For it to come out right, the
link must independently know that ranks 0-3 share a node and that rank 3 and
rank 4 do not — and it must know it for the ranks a QP pair resolves to, which
is a chain of inferences (QP -> LID/QPN -> peer rank -> node) that has never
been exercised on NCCL traffic.

**P4.2 — for every ordered pair `(a,b)` in `comm_edges`, `a/4 != b/4`.** Stated
in rank arithmetic so that it does not consult `scope` at all: this is the same
claim as P4.1 checked against the *launcher's* placement instead of against
comshu's own node derivation. If P4.1 passes and P4.2 fails, `scope` is
self-consistent and wrong.

**P4.3 — the ordered pairs in `comm_edges` are a subset of the connections NCCL
itself reports as `via NET/IB`,** and every rank in `comm_edges` appears in
`nccl_comm` with the one shared `comm_hash`. This is the cross-layer agreement
in its strongest available form: the row set of a derived comshu table judged
against a graph printed by NCCL.

I do **not** predict which pairs those are. NCCL owns ring construction, tree
shape and channel count; it discloses them at run time and llm.c never names
them. Predicting them would be predicting NCCL, not comshu.

**P4.4 — `transport_mix` is empty of NCCL traffic, and the `z0u` arm proves
it rather than assuming it.** `transport_mix` is built from the UCT layer, and
NCCL does not use UCX; its IB transport is its own. So on the `z0` capture the
table is absent or empty. The `z0u` arm loads `ucx,ucx_deep` **as well**, and
llm.c does use MPI — for `MPI_Init`, one broadcast of the NCCL unique id, and
barriers. Prediction: `transport_mix` in `z0u` is **non-empty and carries
kilobytes, not the 3.9 GB of gradient**, so the table that the brief presents as
the answer to "did this leave the node" answers it for the bootstrap and is
blind to the workload. Concretely: `sum(out_bytes+in_bytes)` over
`transport_mix` in `z0u` is **less than 1 %** of `sum(length)` over
`ib_post_send` in the same capture.

That is a scope statement about the tool, not a defect, and it is the answer to
"does `transport_mix` show the intra-node/network split of a gradient
all-reduce": no, and it cannot, because the split happens below a layer it does
not watch.

### 3.2 The volume bound, which is derivable and is not NCCL's business

Per rank the application hands over

    S = 4 steps x G x 2 B (bf16) = 4 x 30 357 504 x 2 = 242 860 032 B

Each node reduces its four GPUs' gradients locally to one vector of S bytes.
Those four node-vectors are mutually independent, so **every node must export at
least S bytes** for its contribution to reach the others, whatever algorithm
NCCL picks. Over four nodes:

**P4.5 — the total InfiniBand payload of the `z0` capture, summed over all 16
ranks, is at least `4S = 971 440 128 B`.**

Calibration, from 04_llmc's 2-node run and not from this one: the same bound
there is `2S = 485 720 064 B` and the measured total was ~535 MB, i.e. 1.10x the
bound. A ring over 16 GPUs would put `4 x 2 x (15/16) x S = 1.82 GB` on the
fabric; a hierarchical or tree algorithm less. I predict the measurement lands
between 1.0 and 2.5 GB and **assert only the lower bound**, because everything
above it is NCCL's algorithm, protocol, channel count and chunking — the same
arithmetic 04_llmc explicitly refused, and refuses again.

`rdma_write_imm` posts carry their payload in the 32-bit immediate and report
`length = 0`; that is correct and is why the bound is on the sum and not on a
per-post minimum (`suite/asserts/sends_have_payload.sql` already documents it).

---

## 4. The identity layer

`-p nccl,ib` at 16 ranks over 4 identical nodes is the first configuration in
this project where a device ordinal is ambiguous four ways.

| table | prediction |
|---|---|
| `rank_identity` | **16 rows**, **4 distinct `node_hash`**, 4 ranks each, and the 4 ranks sharing a hash are `{4n .. 4n+3}` |
| `rank_identity.device` | **non-null on every rank, and equal to `rank % 4`** |
| `nccl_comm` | 16 rows, `nranks = 16`, one shared non-zero `comm_hash`, `cuda_dev = rank % 4` |
| `endpoint` | non-empty; every `rank` in it is 0..15 |
| `link_health` | **0 findings with `n > 0`** |

`rank_identity.device` is the interesting one. 04_llmc round 2 measured it
**null on every rank of every capture**, including a `-p nccl` capture whose
`nccl_comm.cuda_dev` named the device in the same export, and filed it as a
defect. Since then "`rank_identity.device` now resolves on UCX-only captures".
**P5.1 predicts it resolves here too.** If it is still null on an NCCL capture,
the fix did not reach this lane and 04_llmc's finding stands unchanged at four
times the scale.

### `gpu_device`, type 528 — predicted absent, and I would rather be wrong

`libcomshu/schema/comshu.fbs:210` declares `GpuDevice` at type 528: *"What a
device ORDINAL actually refers to, on this rank"*, carrying a UUID and a PCI
address. Measured on the login node **before any capture existed**: `comshu types`
for this build prints 58 rows and **528 is not one of them**, though 524
(`node_identity`), 526 (`peer_mapping`) and 530 (`gpu_kernel`) all are.

**P5.2 — no `gpu_device` stream in `comshu read` and no `gpu_device` table in the
export.** If it appears anyway, then `comshu types` — documented as
*"type-id -> table name, for this build"* — is incomplete, which is a worse
finding than the table being missing, because it is the one place a user is told
to look.

**And if it does appear, here is what it must contain**, so that the check is
pre-registered either way. Four nodes of identical hardware, four devices each:

* 16 rows, one per rank;
* **16 distinct UUIDs** — a GPU UUID is globally unique;
* **exactly 4 distinct PCI addresses**, each appearing on exactly 4 ranks, one
  per node — because the four nodes are the same machine and the bus address of
  the n-th H100 is the same on all of them.

That second pair of lines is the whole reason the record exists, and 16 GPUs
across 4 identical nodes is the smallest configuration that can demonstrate it.

---

## 5. Named failure modes

1. **The loader dies** with `shard_ntok >= ...` — `-b 1` was still too big, or
   the shard files differ from 04_llmc's. Nothing else runs; the bare arm
   catches it.
2. **`comm_edges` is empty on a `-p nccl,ib` capture.** Then the verbs lane
   cannot resolve a peer rank for NCCL's queue pairs, and P4.1-P4.3 are all
   unanswerable at once. That is claim (4) of `apps/README.md` and would be the
   study's headline instead: the derived topology table is blank for the
   commonest multi-node GPU workload there is, while `ib_post_send` in the same
   export holds tens of thousands of rows.
3. **`comm_edges` non-empty with `scope='intra-node'` rows.** Either NCCL used
   IB loopback within a node (which `NCCL_DEBUG=INFO` would show as `via NET`
   for a same-node pair, and would make the rows correct) or the node derivation
   is wrong. The two are distinguishable and the bare arm's output is what
   distinguishes them.
4. **`nccl_collective` counts differ from §2.1.** At P=4 and P=8 they were exact.
   A difference at P=16 with the same flags means either `-b 1` changed
   something I claimed it could not, or the provider drops rows under load.
5. **The `kernel` backend does not load on MN5.** `-p nccl,ib,kernel` refuses
   loudly (04_llmc round 2 measured that it exits 1 rather than capturing
   nothing). P3.2 goes unanswered; nothing else does.

## 6. Guesses that are NOT assertions

* I expect NCCL to pick a tree or hierarchical algorithm rather than a flat
  16-way ring at this size, so I expect the measured IB total nearer 1.0 GB than
  1.8 GB. No assertion depends on it.
* I expect all 16 ranks to post on IB, not only the ones owning a cross-node
  ring hop — 04_llmc's NOTES.md records that at 8 ranks and calls it a wrong
  guess corrected. Recorded as an expectation so that a *change* at 16 ranks is
  visible.
* I expect `-p nccl,ib,kernel` to run in one pass with no replay, since all
  three are hook providers.
* I expect `endpoint.peer_rank` to be populated for NCCL's queue pairs (that is
  the lane `comm_edges` needs) and I have no idea whether the LID/QPN pairing
  survives four nodes; if it half-resolves, the resolution *rate* is the
  measurement.
