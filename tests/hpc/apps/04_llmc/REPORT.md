# 04_llmc — llm.c under `comshu -p nccl` and `-p nccl,ib`

**Application:** [karpathy/llm.c](https://github.com/karpathy/llm.c), `train_gpt2.cu`,
upstream `f1e2ace`. GPT-2 pretraining in C/CUDA, NCCL directly, MPI only to
bootstrap the communicator.
**Runs:** du04, 1 node x 4 A100, `-p nccl` — and MareNostrum 5 job **45082117**,
2 nodes x 4 H100, `-p nccl,ib`. Three configurations on each, plus a bare run.
**Prediction:** [`PREDICTION.md`](PREDICTION.md), written and committed before
either machine was run.
**Result:** every predicted quantity matched, at both rank counts, first attempt.
One assertion of mine was wrong; the capture was right (§5).

---

## 1. The capture is exactly right, against an oracle that is 3000 lines of somebody else's code

Every collective count, element count, datatype and group boundary below was
derived from `llmc/zero.cuh` and `train_gpt2.cu` **before the workload ran**.

| per rank, 4 steps | predicted | du04, P=4 | MN5, P=8 |
|---|---|---|---|
| `nccl_collective` rows, `-z 0` | 311 | **311** | **311** |
| `nccl_collective` rows, `-z 1` | 619 | **619** | **619** |
| bf16 gradient collectives | 304 | **304** | **304** |
| bf16 Σ`count`, `-z 0` | 121 430 016 | **121 430 016** | **121 430 016** |
| bf16 Σ`count`, `-z 1`, per op | `4·G/P` | **30 357 504** | **15 178 752** |
| f32 `all_reduce` rows, `-z 0` / `-z 1` | 7 / 11 | **7 / 11** | **7 / 11** |
| `nccl_group` rows, `-z 0` / `-z 1` | 56 / 184 | **56 / 184** | **56 / 184** |
| `nccl_p2p` rows | 0 | **0** | **0** |
| distinct CUDA streams per rank | 2 | **2** | **2** |
| `nccl_comm` rows / distinct `comm_hash` | P / 1 | **4 / 1** | **8 / 1** |

The element total is the strongest single number. It is

    G = L·(12·C² + 13·C) + Vp·C + T·C + 2·C  =  30 357 504

assembled from the twelve `nelem[]` entries of the backward pass
(`train_gpt2.cu:938-945`) and four more at the end of it (`:963`), with
`L=6, C=384, Vp=50304, T=1024` fixed by `-e d6 -t 1024`. llm.c independently
prints `num_parameters: 30357504`. Four steps of stage-0 all-reduce is
`4G = 121 430 016` elements per rank, and that is what the capture holds — on
both machines, to the element.

**The two rank counts are the point of running twice.** At stage 1 the per-call
count is `pointers_sizes[i] / num_processes`, so P=4 and P=8 must give
`4G/4 = 30 357 504` and `4G/8 = 15 178 752`. They do. A single rank count could
not distinguish a correct `/P` from a constant that happened to fit.

**Why this oracle is stronger than a wire-level one.** The `nccl` provider
records the calls the *application* made. NCCL's channel count, protocol,
algorithm and chunk size decide what crosses the wire, and none of them can
change how many times llm.c called `ncclAllReduce` or with what `count`. So
`colls_per_iter · iters` and `elems · iters` are *derived*, not observed — the
distinction `cases/20_nccl_collectives/meta.json` draws for a workload we wrote,
here made against one we did not.

**The assertions are live, not vacuous.** Mutating one oracle input by one turns
exactly the relevant assertion red and leaves the rest green:

| mutation | capture | result |
|---|---|---|
| `iters` 4 → 5 | z0 | `collective_algebra` + `group_boundaries` FAIL |
| `elems` 30357504 → 30357503 | z0 | `collective_algebra` FAIL, groups ok |
| `colls_per_iter` 76 → 75 | z1 | `collective_algebra` FAIL, groups ok |
| `groups_per_iter` 7 → 8 | z1 | `group_boundaries` FAIL, algebra ok |

## 2. The knob: `-z 1` changes the collective *algebra*, and the trace shows what it did

Claim (3) of the brief, and the headline. One flag, everything else identical —
same binary, same data, same steps, same launcher. `llmc/zero.cuh:531-550` is a
two-way branch inside one `ncclGroupStart`/`End`:

```c
if (config->zero_stage == 0) {
    ncclAllReduce(pointers[i], pointers[i], pointers_sizes[i], ncclFloatX, ncclAvg, ...);
} else if (config->zero_stage == 1) {
    size_t shard_size = pointers_sizes[i] / config->num_processes;
    ncclReduceScatter(pointers[i], pointers[i] + shard_offset, shard_size, ncclFloatX, ncclAvg, ...);
}
```

and `train_gpt2.cu:1108-1119` adds the other half — an `ncclAllGather` per
parameter tensor — only at stage 1. Per rank, four steps, MN5 P=8:

| | `-z 0` | `-z 1` |
|---|---|---|
| `all_reduce` bf16 | 304 rows, Σcount 121 430 016 | **0** |
| `reduce_scatter` bf16 | 0 | **304 rows, Σcount 15 178 752** |
| `all_gather` bf16 | 0 | **304 rows, Σcount 15 178 752** |
| `all_reduce` f32 `avg` (loss) | 4 rows | 4 rows |
| `all_reduce` f32 `sum` | 3 rows | **7 rows** |
| `ncclGroupStart` / `End` | 28 / 28 | **92 / 92** |

Four things the trace states that llm.c's own output only asserts:

- **The all-reduce is gone, not supplemented.** Zero bfloat16 `all_reduce` rows
  survive into a stage-1 run. An implementation that had merely *added* sharding
  would show both, and the flag would be costing rather than saving. Asserted
  explicitly, because it is the failure a volume-only comparison would miss.
- **Each collective shrank by exactly `1/num_processes`.** 304 collectives either
  way, but Σcount falls from `G` to `2G/P` per step: 2× at P=4, 4× at P=8.
  ZeRO-1 breaks even at two processes and only pays from three up — a fact that
  is in the trace and in neither llm.c's log nor its README.
- **A collective appears that has no stage-0 counterpart.** The f32 `sum`
  all-reduces go 3 → 7, one extra per step. It is `gpt2_calculate_grad_norm`
  (`train_gpt2.cu:1022`), compiled into both builds but reachable only at
  stage 1: at stage 0 every rank already holds the full averaged gradient and
  needs no cross-rank norm. The trace surfaces a *conditional* call site that
  the source makes easy to walk past.
- **The fusion structure changed shape, not just size.** Groups per step go 7 →
  23: `L+1` from the backward pass plus `NUM_PARAMETER_TENSORS = 16` from the
  optimizer. ZeRO-1 turns 7 NCCL launches per step into 23 — a threefold rise in
  launch count bought against a fourfold cut in volume. Nothing else in a capture
  can supply that: no send row says whether it was fused with the next, which is
  why `nccl_group` exists.

## 3. The cheap axis: `-d` doubled changes nothing, and that is the claim

`grad_accum_steps = total_batch_size / (B·T·P)` (`train_gpt2.cu:1519`), and
`multi_gpu_async_reduce_gradient` is called only under `if(last_step)`
(`train_gpt2.cu:929`, `:953`). So the reduction fires once per *optimizer* step
however many micro-steps fed it.

    du04, P=4:  -d 8192   grad_accum_steps=1   8.69 ms/step   311 collectives, 56 groups
                -d 16384  grad_accum_steps=2  15.65 ms/step   311 collectives, 56 groups

Wall time 1.80×, collective count 1.00×, group count 1.00×, Σcount 1.00×. The
MN5 pair agrees on every count; its step times (6–25 ms) are too noisy at this
model size to quote. The invariance is not a separate assertion — it is that the
*same* assertion files pass unmodified on the doubled capture, which is what the
claim actually means.

## 4. A defect in llm.c, visible because the capture is faithful

`asserts/scalar_reduction_count_is_sizeof_float.sql`. Four sites pass
`sizeof(float)` where `ncclAllReduce` takes an element count:

    zero.cuh:483        multi_gpu_barrier
    zero.cuh:588        multi_gpu_cpu_float_sum
    train_gpt2.cu:958   final loss reduction
    train_gpt2.cu:1022  gradient-norm reduction (ZeRO-1 only)

Each reduces **4 floats where the code means 1**, and the capture says so:

    rank,ticks,op,comm,sendbuff,recvbuff,stream,count,datatype,elem_bytes,logical_bytes,redop,root
    2,48578586296109278,all_reduce,1,2,2,3,4,float32,4,16,sum,-1

`count = 4`, `logical_bytes = 16`, on every f32 row of every rank of every run on
both machines. Three of the four are benign — element 0 still gets the right
answer and the other three land in slack inside a larger buffer. The fourth is
not:

```c
cudaCheck(cudaMallocManaged(&result.unified_buffer, sizeof(float)));   // zero.cuh:455
...
ncclAllReduce(config->unified_buffer, config->unified_buffer, sizeof(float), ...)  // :483, :588
```

Four bytes of managed memory, sixteen bytes written, on every
`multi_gpu_barrier` and every `multi_gpu_cpu_float_sum` — a 12-byte heap
overflow present at upstream `f1e2ace`. A bug in llm.c, not in comshu, which is
why it is here and not in `BUGS-FOUND.md`.

The method matters more than the bug: **the assertion expects 4 and passes.** `4`
is `sizeof(float)`, an oracle from the C language. Had the capture shown `1` —
the value every variable name in that code invites — the provider would have been
inventing a number, and no amount of the capture agreeing with itself would have
shown it.

## 5. One assertion of mine was wrong. The capture was right.

`ib_carries_the_inter_node_hops` demanded that every recorded IB send carry
payload. On job 45082117 it produced **11,000 offending rows per stage-0 rank**,
and every one of them was correct capture:

    opcode          n      zero_len  min_len  max_len
    rdma_write      22558  0         64       75776
    rdma_write_imm  11558  11000     0        8192

NCCL's inter-node path posts `rdma_write_imm`, whose payload *is* the 32-bit
immediate, so length 0 is the right value there. `rdma_write` never reaches 0 —
it ranges 64 B to 75 776 B. The suite's own cross-cutting
`suite/asserts/sends_have_payload.sql` already carries exactly this exclusion,
cites `02_nccl_internode` posting 1120 such sends per rank, and reported `[ok]`
on the same capture that my assertion failed on.

The assertion has been restated to claim what is actually true — *a zero-length
send that is not a write-with-immediate is a decode error* — with the history in
the file. This is **not** the forbidden move of editing an assertion to match
what comshu emitted: the emission is independently corroborated by the RDMA verb's
semantics, by `rdma_write`'s disjoint length range in the same capture, and by an
assertion written months earlier for a different case. What was wrong was my
claim about the protocol.

A second pre-registered guess was also wrong, and it cost nothing because no
assertion depended on it: `PREDICTION.md` says only the ranks owning a cross-node
ring hop would post on IB, by analogy with `07_nvshmem_multinode`'s asymmetry.
All eight posted — NCCL builds several rings and gives every local GPU an
inter-node connection. The assertion deliberately said nothing about *which*
ranks, which is the whole difference between it and the prediction.

## 6. Intent and wire, one process, one execution

`-p nccl,ib` composes without replay — both are hook providers — so the same run
holds the collective llm.c asked for and the traffic NCCL turned it into. Stage 0,
MN5, per rank, 4 steps:

| rank | IB sends | IB bytes | max send |
|---|---|---|---|
| 0, 4 | 4578 | 62 853 536 | 75 776 |
| 1, 5 | 4752 | 68 259 840 | 75 776 |
| 2, 6 | 3936 | 68 241 408 | 75 776 |
| 3, 7 | 3792 | 68 189 184 | 75 776 |

Every rank that posted on IB also appears in `nccl_comm` with the one shared
`comm_hash`, so the two providers' rank spaces agree — asserted, and the only
part of this table that is a claim.

**No IB quantity is asserted against `elems`, deliberately.** The application
handed over `4G·2 B = 242.9 MB` of logical gradient per rank; the wire carries
63–68 MB per rank in 3792–4752 posts, each of a size NCCL chose. That ratio is
NCCL's algorithm, protocol, channel count and chunking, none of which llm.c
names — the same reason `02_nccl_internode`'s `workload_note` refuses the
arithmetic. What the pairing gives is the *shape*: **a third of the posts by
count** are notification traffic (11 558 of 34 116 are `rdma_write_imm`, and
11 000 of those carry no payload at all), while **essentially every byte** is
`rdma_write` — at most 4.6 MB of the 535 MB total could be immediate-bearing
writes. That is a statement about NCCL's proxy protocol which neither provider
makes alone: `nccl` sees 304 all-reduces and no posts, `ib` sees 34 116 posts and
no all-reduces.

---

## What comshu got wrong or is missing

**In the `nccl` provider: nothing.** Every field checked — `op`, `datatype`,
`count`, `redop`, `root`, `comm`, `stream`, `depth`, `nranks`, `nccl_rank`,
`cuda_dev`, `comm_hash` — agreed with the application's source at two rank
counts, on two GPU generations, in three configurations. The capture also does
not perturb the workload: the bare and captured runs print identical losses to
six decimals (`10.882606`, `9.667173`).

Three smaller things, none of them capture defects:

1. **`providers/nccl/CMakeLists.txt:10` needs `-DCUDA_RT_INCLUDE_DIR` spelled out**
   on any host where the CUDA toolkit is outside CMake's default prefix path —
   du04's `/usr/local/cuda/include` is one. `find_path(... REQUIRED)` fails
   configuration with a message that does not suggest the fix. A first-run cost,
   recorded in `NOTES.md`.

2. **An empty table and a missing table are still hard to tell apart at the
   assertion layer.** `-- requires: nccl_p2p` reports `[skip]` for an
   empty-but-present table, so an assertion that *wants* to claim emptiness must
   not require its own table — and if that table were genuinely absent, the query
   would fail to parse and read as a pass. Same edge as case 20's job 44724071
   and case 08's empty `ib_queue_pair`. It bit nothing here only because
   `comshu export --csv` writes a header-only CSV for every known record type: the
   safety comes from the exporter, not from the assertion mechanism.

3. **`meta.json`'s `workload` block cannot describe two topologies.** `PARAM_COLS`
   in `sql_check.sh` is a fixed, closed list that rejects unknown keys, so
   running one case at P=4 and P=8 needs the case directory projected into a temp
   copy with `ranks`/`nodes` rewritten (`run.sh` does this). Fine for a study; a
   case that legitimately runs at two scales would want harness support.

### The open questions from `apps/README.md`

- **Managed memory (item 2) — named, not answered.** llm.c allocates managed
  memory in two places: `cudaMallocConditionallyManaged` (`llmc/cuda_utils.cuh:213`),
  an OOM fallback for the AdamW state that did not fire at this model size, and
  `cudaMallocManaged(&unified_buffer, sizeof(float))` (`zero.cuh:455`), which
  always fires. The second is a **four-byte managed buffer used directly as an
  NCCL reduction target and read back on the host every barrier** — about the
  smallest possible test of whether `mem_object.device` means anything for
  migrating memory, and llm.c is the first workload available to this project
  that has one. It needs a `-p gpumem` pass, which this study deliberately did
  not take (NVBit's ~440 s module cost on NCCL, per case 05). That is the obvious
  follow-up and it is cheap to state precisely: `-e d6 -x 1 -p gpumem`, then ask
  whether the 4-byte allocation is in `mem_object` at all and what `device` says.
- **UCX → verbs attribution.** Not applicable — llm.c bootstraps with MPI and
  communicates with NCCL; there is no UCX layer here to correlate.
- **Unattributed GPU accesses (item 4).** Not exercised; no `gpumem` pass.

---

## Reproducing

```bash
# du04, no queue
bash tests/hpc/apps/04_llmc/run.sh

# MN5, one job at a time
while squeue -u koc296797 -h | grep -q .; do sleep 60; done
sbatch tests/hpc/apps/04_llmc/job.sbatch
```

Both run the workload **bare first** (`bare exit=0`, val loss 10.87 → 10.12),
then capture. Data is tinyshakespeare (`dev/data/tinyshakespeare.py`, 305 260
train / 32 768 val tokens), tokenized off-cluster and rsync'd. Build traps —
including the one-line CUDA-13 patch llm.c needs to compile at all on du04 — are
in `NOTES.md`. Job 45082117 took **80 s** wall for four configurations on two
nodes; the `nccl` provider is a host-API interposer and costs milliseconds.
