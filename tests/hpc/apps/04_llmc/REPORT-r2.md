# 04_llmc round 2 — llm.c under `-p nccl,kernel`, through `comshu link`

**Application:** [karpathy/llm.c](https://github.com/karpathy/llm.c), `train_gpt2.cu`,
upstream `f1e2ace` + the CUDA-13 patch in this directory. GPT-2 pretraining in
C/CUDA, NCCL directly, MPI only to bootstrap.
**Machine:** du04, 1 node x 4 A100-SXM4-40GB, NCCL 2.28.9 over NVLink. MN5 was
unreachable for this round, so everything here is P=4 single-node.
**Prediction:** [`PREDICTION-r2.md`](PREDICTION-r2.md), committed in `66a02f5`
**before** the first r2 capture existed.
**Written black-box:** no file under `providers/`, `libcomshu/` (bar the schema),
`comshu-read/`, `tools/`, `plans/` or `BUGS-FOUND.md` was read. Every question
that could not be answered from the capture and its documentation is in
[`QUESTIONS.md`](QUESTIONS.md) rather than resolved by reading the source.

**Headline.** The cross-layer prediction is exact: llm.c's collectives become
**35 NCCL device kernels per rank at `-z 0` and 103 at `-z 1`**, the numbers
derived from its source before the run, and the capture holds precisely those.
Against that, one derived table is **wrong** — `rank_identity.device` is null on
every rank of every capture while two raw tables in the same export name the
device — and one derived table is **blank where a user would look first**:
`comm_edges` has no row for a workload that moved 243 MB per rank.

---

## 1. Prediction against measurement

Per rank, four optimizer steps, P=4. Everything in the "predicted" column was
committed before the run.

| | predicted | measured | |
|---|---|---|---|
| `nccl_collective` rows, `-z 0` / `-z 1` | 311 / 619 | **311 / 619** | ok |
| bf16 sum(`count`), `-z 0` | 121 430 016 | **121 430 016** | ok |
| bf16 sum(`count`) per op, `-z 1` (`reduce_scatter`, `all_gather`) | 30 357 504 each | **30 357 504 each** | ok |
| f32 `all_reduce` rows, `-z 0` / `-z 1` | 7 / 11 | **7 / 11** | ok |
| every f32 `count` | 4 (`sizeof(float)`) | **4** | ok |
| `nccl_group` rows, `-z 0` / `-z 1` | 56 / 184 | **56 / 184** | ok |
| distinct streams in `nccl_collective` | 2 | **2** | ok |
| **NCCL device kernels, `-z 0`** | **35** | **35** | ok |
| **NCCL device kernels, `-z 1`** | **103** | **103** | ok |
| streams carrying them | 2 | **2** | ok |
| `gpu_kernel.device` vs `nccl_comm.cuda_dev` | equal | **equal, 0..3** | ok |
| `link_health` findings with n > 0 | 0 | **0** (of 18 rows) | ok* |
| `comm_edges` rows | 0 | **0** | ok |
| `transport_mix`, `endpoint`, `object_access`, `tag_flow`, `tag_recv_resolved` | 0 | **0** | ok |
| `rank_identity` rows | 4 | **4** | ok |
| `rank_identity.device` = the rank's GPU | yes | **null, label `n0 · GPU ?`** | **WRONG, §3** |
| `-p nccl,kernel` runs one pass, no replay | yes | **"2 providers -> 1 pass"** | ok |
| knob B: kernel names change with `NCCL_PROTO` | yes | **no — §5** | **wrong** |
| §5 guess: `-z 1` costs less device time than `-z 0` | lower | **32 % higher** | **wrong** |

\* the prediction was "`link_health` is empty" because the brief says so. It is
not empty and never can be: see §6.

## 2. The capture is right, at a layer round 1 could not reach

`grep -rn gpu_kernel tests/hpc/cases/*/asserts/*.sql` is empty: before this run,
nothing in the project had ever asserted a property of the `kernel` provider.
Three claims, all derived from llm.c's source before the capture existed.

**(a) One group is one kernel.** llm.c says it — `zero.cuh:530`, *"NCCL group:
aggregate all pointers in a single NCCL GPU kernel"* — and `comshu.fbs` type 906
says it from the other side, *"N grouped sends are ONE launch"*. Neither is a
capture. So, per rank, `launches = nccl_group/2 + ungrouped collectives`, and
the ungrouped ones are exactly the float32 scalar reductions because all four
f32 call sites sit outside a group:

| | groups/step | grouped | ungrouped f32 | predicted | **measured** |
|---|---|---|---|---|---|
| `-z 0` | 7 | 28 | 7 | 35 | **35** |
| `-z 1` | 23 | 92 | 11 | 103 | **103** |

and the breakdown matches call site by call site: at `-z 1` the 92 split as
**28** `ncclDevKernel_ReduceScatter_Sum_bf16` (4 steps x `L+1` groups) and **64**
`ncclDevKernel_AllGather` (4 steps x `NUM_PARAMETER_TENSORS = 16` groups from
`train_gpt2.cu:1110`). A tracer that recorded launches without group structure
could not produce those two numbers; NCCL's planner splitting a fused group —
the one non-defect way this could have come out high, named in the prediction —
did not happen.

**(b) The two providers agree on which GPU a rank is.** `nccl_comm.cuda_dev` and
`gpu_kernel.device` both give 0,1,2,3 for ranks 0..3, one device per rank, which
is what `cudaSetDevice(local_device_idx)` at `zero.cuh:448` fixes.
(`asserts/kernel_device_matches_nccl_rank_r2.sql`.)

**(c) Each scalar reduction lands on the stream its call site names.** This is
strictly stronger than round 1's "two distinct streams", which a provider
stamping a constant pair of handles would also satisfy. llm.c has two streams
and four f32 sites, and the source says which uses which. Measured, rank 0,
`-z 0` / `-z 1`:

| stream | `nccl_collective` | `gpu_kernel` (NCCL) | call sites |
|---|---|---|---|
| nccl_stream | 304 bf16 + **3** f32 | 28 bf16 + **3** f32 | `zero.cuh:530-551`, and `:483`/`:588` |
| main_stream | **4** / **8** f32 | **4** / **8** f32 | `train_gpt2.cu:958`, and `:1022` at ZeRO-1 only |

The **3** is an oracle from the command line, not from the run: one
`multi_gpu_cpu_float_sum` from `gpt2_allocate_state` (`train_gpt2.cu:412`) plus
one per validation, and `-v 1000 > -x 4` puts a validation at step 0 and at the
last step only. Both layers independently reproduce the same 3/4 and 3/8 split.

**Independent corroboration from NCCL itself.** `NCCL_DEBUG_SUBSYS=TUNING` on
the same command line prints one line per collective. Its byte sizes are
llm.c's element counts times two, including `38 633 472 Bytes` = `Vp*C*2` for
the token embedding, and — the round-1 llm.c defect, confirmed by a third
party — **`AllReduce: 16 Bytes` exactly 7 times per rank**, which is
`sizeof(float)` floats reduced where the code means one.

**Assertions are live.** Mutating one oracle input by one turns exactly the
relevant assertion red (`check-r2.sh` over `z0.pq`):

| mutation | result |
|---|---|
| `iters` 4 -> 5 | `one_launch_per_group` + `scalar_reductions` FAIL, rest unchanged |
| `groups_per_iter` 7 -> 8 | `one_launch_per_group` FAIL only |
| `ranks` 4 -> 3 | `kernel_device_matches_nccl_rank` FAIL only |

## 3. The capture is wrong: `rank_identity` does not name the device

`asserts/rank_identity_names_the_device_r2.sql`, and it **fails on all four
captures**, including the `-p nccl`-only one.

    rank,node_ix,node_hash,device,devices,label
    0,0,5641944787155511676,,0,n0 · GPU ?
    1,0,5641944787155511676,,0,n0 · GPU ?
    2,0,5641944787155511676,,0,n0 · GPU ?
    3,0,5641944787155511676,,0,n0 · GPU ?

The same export answers the question twice:

    SELECT c.rank, c.cuda_dev, k.dev FROM nccl_comm c
      JOIN (SELECT rank, min(device) dev FROM gpu_kernel GROUP BY rank) k USING (rank);
    -- 0,0,0   1,1,1   2,2,2   3,3,3

`rank_identity` is the table whose *purpose* is to hold (node, device) per rank —
`comshu.fbs`'s `NodeIdentity` comment states the need in as many words: *"a
device ordinal is node-local... resolving a peer device to a rank needs (node,
device), and this is the node half"*. The node half is present and correct
(one `node_hash`, shared, `node_ix` 0). The device half is null, `devices` is 0,
and the label prints a literal `?`.

**Smallest reproduction** (about 90 s on du04, no queue):

```bash
mpirun -np 4 comshu run -p nccl -o OUT -- ./train_gpt2cu -e d6 -b 2 -t 1024 -x 1 -z 0 -d 8192
comshu export OUT --parquet OUT.pq && comshu link OUT.pq
comshu sql OUT.pq "SELECT * FROM rank_identity"
```

Any NCCL workload reproduces it; nothing about llm.c is needed beyond four ranks
on four GPUs.

**Why it matters more than a blank column.** Nothing reports it: `link_health`
shows 18 findings, every one `n = 0`, on the same export — so the link's own
verdict is "healthy" while a key column is empty. A consumer joining on
`rank_identity.device` gets no rows and no error, which is the exact failure
mode the schema README calls the worst case in the format.

**What I can say about the cause, without reading the code:** the column is
equally null in the `-p nccl` capture, which has no `gpu_kernel` at all, and in
the `-p nccl,kernel` captures, which have 1251 kernel rows per rank naming the
device. So the device lane consults neither `nccl_comm.cuda_dev` nor
`gpu_kernel.device`. `mem_object` and `gpu_access` are both empty here, and they
are the tables a `gpumem` capture would fill — the plausible reading is that
`rank_identity.device` is fed only from the NVBit-side tables, so every capture
without a `gpumem` pass gets `GPU ?`. That is a hypothesis about the link, not a
finding; the finding is the null column and the silent `link_health`.

## 4. A door that is missing, named: collectives reach no edge table

`comm_edges` is **empty** for a run in which each rank pushed
`4G x 2 B = 242.9 MB` of gradient through NVLink. That was predicted, and it is
correct behaviour rather than a bug: llm.c calls `ncclSend`/`ncclRecv` zero
times, `comshu.fbs` type 904 says `NcclP2p` is *"the only NCCL call that names a
peer, and so the only one an exact edge follows from"*, and this study loaded no
`ib`, no `ucx` and no `gpumem`. Inventing a ring from an `ncclAllReduce` would
be a fabrication — NCCL owns algorithm, channel count and chunking — and
`asserts/collective_only_means_no_invented_edges_r2.sql` states that as the
claim, so that a future link which grows a collective lane must say what the
lane means instead of quietly filling the table.

But state the consequence plainly, because it is claim (4) of `apps/README.md`:
**for the single most common shape of GPU-to-GPU traffic there is — a
data-parallel all-reduce on one node — the derived table the suite treats as
*the* topology view has nothing in it, and no other derived table has anything
either.** `transport_mix`, `endpoint`, `object_access`, `tag_flow` and
`tag_recv_resolved` are all empty too. Everything this study learned about the
traffic came from `nccl_collective`, `nccl_group` and `gpu_kernel` — three raw
tables — plus `timeline_span`, which is the one derived table that carried
anything (5004 rows at `-z 0`, exactly the 4 x 1251 kernels).

What would close it, stated as precisely as I can from outside: an `nccl` capture
knows *(comm_hash, nccl_rank, nranks)* for every rank and *(op, count, datatype)*
for every collective. That is enough to say "these four ranks exchanged this
logical volume in this collective", which is a hyper-edge, not a src->dst pair —
`comm_edges`' six columns cannot express it without choosing a topology NCCL
never disclosed. The honest shapes are a separate `collective_edges`
(comm_hash, op, participants, logical bytes) or an explicit `transport =
'nccl-collective'` lane with `dst_rank` null. Either is a design decision, which
is why this is a finding and not a patch.

## 5. The knob: `NCCL_PROTO` changes execution 2.9x and the capture shows it — but not in the field a user would look at

Same binary, same command line, same `-z 0`, one environment variable pair
(`NCCL_ALGO=Ring NCCL_PROTO=Simple`). Per rank, four steps:

| | default | forced Simple | ratio |
|---|---|---|---|
| `nccl_collective` rows / sum(`count`) | 311 / 121 430 016 | **311 / 121 430 016** | 1.00 |
| `nccl_group` rows | 56 | **56** | 1.00 |
| NCCL device kernels | 35 | **35** | 1.00 |
| **gradient-kernel device time, rank 0..3 (ms)** | 5.32 / 5.67 / 5.49 / 6.08 | **15.51 / 15.63 / 15.51 / 15.60** | **2.7 – 2.9x** |
| all-kernel device time per rank (ms) | 53.6 | **66.4** | 1.24 |
| step wall time (llm.c's own log) | 8.72 ms | **10.08 ms** | 1.16 |

That is claim (3) of `apps/README.md`: the application's own log says only
"slower"; the trace says *where* — the extra 10 ms per rank is entirely inside
the NCCL gradient kernels, the launch structure is untouched, and most of the
2.5 ms/step device increase is hidden by compute overlap on the other stream,
which is what `multi_gpu_async_reduce_gradient`'s event handshake
(`zero.cuh:525-528`) exists to buy.

**My prediction that the kernel *names* would change was wrong, and the reason
is worth more than the prediction.** In both arms every NCCL kernel is named

    _Z40ncclDevKernel_AllReduce_Sum_bf16_RING_LL24ncclDevKernelArgsStorageILm4096EE

The independent oracle is NCCL's own tuner. With `NCCL_DEBUG_SUBSYS=TUNING`, the
default arm reports **three different protocols within the same fused group** —
`768 Bytes -> proto LL`, `1179648 Bytes -> proto LL128`,
`38633472 Bytes -> proto SIMPLE` — while the forced arm reports `SIMPLE` for
every one. Yet each of those groups is exactly **one** kernel launch, which this
study measured independently in §2. One kernel cannot be specialised for three
protocols, so **the symbol NCCL launches names one compiled specialisation and
not the protocol its work items actually take**. comshu records the launched
symbol faithfully — the proof is that `-z 1` shows three *different* NCCL kernel
names in the same capture, so nothing is being cached or reused.

The consequence is a limit on what any CUPTI-level capture can say, and it
belongs in the docs rather than in the code: **`gpu_kernel.name` does not
identify the algorithm or protocol of a fused NCCL collective.** A user reading
`..._RING_LL` off this capture and concluding "llm.c ran LL" would be wrong for
two of the three protocols actually used. Nothing in the capture can correct
that — no record carries NCCL's per-collective tuning decision — and it is in
`QUESTIONS.md` as the single largest thing the capture could not tell me.

## 6. Two things the documentation says that the tool does not do

**`link_health` is never empty.** `BRIEF-BLACKBOX.md` says it "is expected to be
empty. Every row in it is a defect in the link". It is a fixed rubric of **18
named findings**, always written in full, each with a count and a note; health is
`n = 0` on all 18. Following the brief literally, a clean capture files 18
defects. Case 21's own assertion already tests `n > 0` for one finding, so the
implementation and the suite agree with each other and the brief is the odd one
out. `asserts/link_health_reports_no_finding_r2.sql` states the working version.
On these captures all 18 are zero — including the four this workload actually
exercises (`timeline_unanchored`, `kernel_rows_submission_only`,
`unresolved_nvlink`, `wr_flight_backwards`).

**The two exporters disagree about empty tables, and the derived layer is a
third behaviour.** `comshu export --csv` writes a header-only file for every known
record type (58 files here, 55 of them empty). `comshu export --parquet` writes a
raw table only when it has rows, so `SELECT count(*) FROM nccl_p2p` is a hard
`Catalog Error` on a Parquet export of the very same capture. `comshu link`
writes its derived tables out **even when empty** — `comm_edges` is a real table
with zero rows in the same directory where `nccl_p2p` does not exist at all.
Since `comshu link` accepts only Parquet ("no raw .parquet tables in ...csv — run
`comshu export --parquet` first"), any assertion over the derived layer lives on
the representation where an empty raw table is a parse error, and a parse error
is the failure mode round 1 already flagged as reading like a pass. This is
`apps/README.md`'s "four different nothings", made sharper: an empty table, a
missing table and a missing export format now all look different.

## 7. Cost, and non-perturbation

| | step time | kernels/rank | CUPTI dropped |
|---|---|---|---|
| bare | 8.72 ms | — | — |
| `-p nccl` | 8.72–8.82 ms | — | — |
| `-p nccl,kernel` | 8.91 ms | 1251 (`-z 0`), 1379 (`-z 1`) | **0** |

Adding CUPTI costs ~2 % of step time and drops nothing; adding it changes no
NCCL number (`z0` and `z0nk` agree row for row, which is what the `z0nk` arm is
for). Bare and captured runs print identical losses to six decimals
(`10.874087`, `9.528946`). `-p nccl,kernel` is **one pass** — comshu says so
(`2 providers -> 1 pass`) and llm.c's banner appears once — so the cross-layer
join in §2 is within a single execution.

## 8. What ZeRO-1 costs at four ranks, which llm.c's README does not say

Free, from the same captures, and it corrects my own §5 guess:

| per rank, 4 steps | `-z 0` | `-z 1` |
|---|---|---|
| logical gradient volume | `4G` = 121 430 016 elem | `2 x 4G/P` = 60 715 008 elem |
| NCCL device kernels | 35 | 103 |
| gradient-kernel device time | 5.3–6.1 ms | **7.0–7.3 ms** |

Half the logical bytes, 2.94x the launches, and **32 % more device time**. ZeRO-1
does not pay for itself at P=4 on one NVLink node, and the trace is where that is
visible: the count halved exactly as the source says it must, and the clock went
the other way.

---

## Reproducing

```bash
bash tests/hpc/apps/04_llmc/run-r2.sh                       # du04, no queue, ~2 min
bash tests/hpc/apps/04_llmc/check-r2.sh /DATA/missa18/comshu-apps-r2/z0.pq
```

`run-r2.sh` runs bare first, then `z0`, `z1`, `z0sim` (the NCCL protocol arm) and
`z0nk` (the `-p nccl` control), exporting Parquet and linking each.
`check-r2.sh` materialises `params` from `meta-r2.json` and runs every
`asserts/*_r2.sql`; it exists rather than reusing `suite/sql_check.sh` because
that script reads `meta.json`, which belongs to round 1's 8-rank topology and
must not be edited. Round-1 files are untouched; every r2 artefact carries the
suffix.

**Result:** 5 assertions pass on all four captures, 1 fails on all four and is
marked `expect_fail_asserts` in `meta-r2.json` with the evidence in §3.

**Repeatability.** An independent fifth run of the `-z 0` configuration, exported
and linked separately, reproduces every quantity exactly — 35 NCCL kernels and
1251 kernels per rank, 311 collectives, 121 430 016 bf16 elements, same six
assertion verdicts. The kernel counts are byte-stable across runs even though the
device *times* are not, which is why nothing here asserts a duration.
