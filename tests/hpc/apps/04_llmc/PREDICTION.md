# Pre-registered prediction — written before the MN5 job was submitted

Derived from `llmc/zero.cuh` and `train_gpt2.cu` at upstream `f1e2ace`, plus the
command line. Nothing here was read off a capture. Job submitted after this file
was written; the du04 predictions below were likewise written and computed before
`run.sh` was executed for the first time.

## The derivation

    L  = 6          -e d6            train_gpt2.cu:523
    C  = 384        -e d6            train_gpt2.cu:523
    Vp = 50304      padded vocab     train_gpt2.cu:583
    T  = 1024       -t 1024, = max_seq_len
    S  = 4          -x 4             train_gpt2.cu:1712, 1819
    P  = 8 (MN5, 2 nodes x 4 H100) / 4 (du04, 1 node x 4 A100)

Gradient elements reduced per optimizer step, per rank:

    G = L*(12*C^2 + 13*C) + Vp*C + T*C + 2*C
      = 6*1774464 + 19316736 + 393216 + 768
      = 30357504

which at `T == max_seq_len` is also the model's parameter count — llm.c prints
`num_parameters: 30357504`.

Collectives per optimizer step, per rank:

| site | source | stage 0 | stage 1 |
|---|---|---|---|
| gradient reduction, 12 per layer + 4 non-block | `zero.cuh:531-550` | 76 `all_reduce` bf16 | 76 `reduce_scatter` bf16 |
| optimizer-state gather | `train_gpt2.cu:1108-1119` | — | 76 `all_gather` bf16 |
| final loss | `train_gpt2.cu:958` | 1 `all_reduce` f32 | 1 `all_reduce` f32 |
| gradient norm | `train_gpt2.cu:1022` | — | 1 `all_reduce` f32 |
| **per step** | | **77** | **154** |

Outside the loop, both stages: 1 (`gpt2_allocate_state`, `train_gpt2.cu:412`) +
2 validations (step 0 and the final step, because `-v 1000 > S`;
`train_gpt2.cu:1718, 1727`) = **3** f32 `all_reduce`.

`ncclGroupStart`/`End` pairs per step: `L+1 = 7` from backward
(`zero.cuh:530, 551`, taken at both stages), plus at stage 1 one per parameter
tensor, `NUM_PARAMETER_TENSORS = 16` (`train_gpt2.cu:97, 1110-1118`) = **23**.

## The numbers, per rank

| | z0 (P=8) | z1 (P=8) | z0d2 (P=8) | z0 (P=4, du04) | z1 (P=4, du04) |
|---|---|---|---|---|---|
| `nccl_collective` rows | 311 | 619 | 311 | 311 | 619 |
| — bf16 `all_reduce` rows | 304 | 0 | 304 | 304 | 0 |
| — bf16 `all_reduce` Σcount | 121 430 016 | 0 | 121 430 016 | 121 430 016 | 0 |
| — bf16 `reduce_scatter` rows | 0 | 304 | 0 | 0 | 304 |
| — bf16 `reduce_scatter` Σcount | 0 | 15 178 752 | 0 | 0 | 30 357 504 |
| — bf16 `all_gather` rows | 0 | 304 | 0 | 0 | 304 |
| — bf16 `all_gather` Σcount | 0 | 15 178 752 | 0 | 0 | 30 357 504 |
| — f32 `all_reduce` rows (avg) | 4 | 4 | 4 | 4 | 4 |
| — f32 `all_reduce` rows (sum) | 3 | 7 | 3 | 3 | 7 |
| — every f32 `count` | 4 | 4 | 4 | 4 | 4 |
| `nccl_group` rows | 56 | 184 | 56 | 56 | 184 |
| distinct streams | 2 | 2 | 2 | 2 | 2 |
| `nccl_p2p` rows | 0 | 0 | 0 | 0 | 0 |
| `nccl_comm` rows | 8 | 8 | 8 | 4 | 4 |

`nccl_comm`: one distinct non-zero `comm_hash` across all ranks, `nranks = P`,
`nccl_rank` distinct, each `cuda_dev` claimed by `P/4` ranks.

Every f32 `count` is **4**, not 1, because llm.c passes `sizeof(float)` where
`ncclAllReduce` takes an element count (`zero.cuh:483, 588`;
`train_gpt2.cu:958, 1022`). This is a prediction about a defect: if the capture
showed 1 the provider would be inventing it.

`ib_post_send` (MN5, `-p nccl,ib`): non-empty, but **not** on every rank — with
NVLink P2P enabled only the ranks owning a cross-node ring hop run NCCL's IB
proxy. No prediction is made about the send count or byte total: NCCL owns
algorithm, protocol, channels and chunking, so the wire is its decision and not
llm.c's (the same reason case 02 asserts nothing of the kind). Predicted only:
at least one rank posts, every posted send has non-zero length, and every rank
that posts also appears in `nccl_comm`.

## The knob claims

1. **`-z 0` -> `-z 1` changes the collective algebra, not the volume.** 76
   `all_reduce` per step become 76 `reduce_scatter` + 76 `all_gather`, the
   element count per collective divides by `P`, and a second f32 reduction
   appears per step (the ZeRO-1-only grad-norm all-reduce). The total logical
   element volume per step falls from `G` to `2G/P` — at P=8 that is a 4x
   reduction, at P=4 it is exactly break-even.
2. **`-d` doubled changes nothing in the capture.** `grad_accum_steps` goes 1 ->
   2 and the step wall time roughly doubles, but every count above is identical,
   because the reduction sits under `if(last_step)` and therefore fires once per
   optimizer step regardless of how many micro-steps fed it.

---

## Correction to this file's prose, recorded rather than edited away

Claim 1 above says the stage-1 logical volume `2G/P` is "at P=4 exactly
break-even" against stage 0's `G`. That is an arithmetic slip: `2G/4 = G/2`, a
**2x** reduction, not break-even. Break-even is at `P = 2`.

Left standing with this note rather than silently corrected, because the point
of a pre-registered prediction is that it is not edited after the fact. **No
predicted quantity is affected** — every number in the table, and therefore
everything the assertions check, was computed from `2G/P` and is unchanged. The
error was in the sentence describing them.
