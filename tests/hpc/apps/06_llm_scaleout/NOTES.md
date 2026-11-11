# 06_llm_scaleout — environment traps

llm.c at 16 GPUs across 4 MN5 nodes. `04_llmc/NOTES.md` and
`04_llmc/NOTES-r2.md` still apply in full — the CUDA-13 `cudaMemAdvise` patch,
the Makefile's `dpkg`-gated `-DMULTI_GPU`, the cublas-in-math_libs paths, the
`-lnvidia-ml` stub. These are the ones that are new because the rank count is
sixteen and the nodes are four.

---

## 1. `-b 2` dies in the loader at sixteen ranks, one token short

`llmc/dataloader.h:186`:

```c
assert(shard_ntok >= (int64_t) (num_processes * B * T + 1));
```

on **every** shard. tinyshakespeare's validation split is exactly **32,768**
tokens. At P=16, `B=2, T=1024` needs `32,769`, so the job dies before NCCL is
initialised — and the failure is an `assert`, i.e. an abort with no explanation
of which shard or which arithmetic.

04_llmc hit the same wall at P=8 with `-b 4` and moved to `-b 2`. At P=16 the
answer is `-b 1` (needs 16,385).

**This changes no collective.** llm.c's gradient tensors are model-shaped, not
batch-shaped, so every count in `meta.json` — 304 bf16 rows, 121,430,016
elements, 7 f32 rows, 56 group boundaries — is what it was at P=4 and P=8. The
only P-dependent number is ZeRO-1's `4G/P`.

Worth knowing for anyone going further: **P=32 is the wall.** `32 * 1 * 1024 + 1
= 32,769 > 32,768`, so tinyshakespeare cannot feed thirty-two ranks at any batch
size. A larger dataset has to be tokenized off-cluster and rsync'd; MN5 has no
outbound network.

## 2. `-d` must be a multiple of `B*T*P`, and P changed

`train_gpt2.cu:1518` asserts `total_batch_size % tokens_per_fwdbwd == 0` and
`:1519` sets `grad_accum_steps` from the quotient. 04_llmc's `-d 16384` was
`2*1024*8`; here the same 16384 is `1*1024*16`. It happens to coincide, and it
would not have if either `-b` or the node count had moved by one — so compute it,
do not copy it. `grad_accum_steps = 1` matters because
`gpt2_backward_and_reduce` gates the reduction on
`micro_step == grad_accum_steps - 1` (`train_gpt2.cu:794`), so a different value
changes the collective count per step.

## 3. The cupti backend is not under `providers/`, and it needs libcupti on the path

The `z0k` arm of job 45330280 died at exec, sixteen times:

```
train_gpt2cu: error while loading shared libraries: libcupti.so.12: cannot open
shared object file: No such file or directory     (exit 127)
```

`comshu run` had already found and injected the backend; the missing library is
the backend's OWN dependency, and the error names the *application*, so it reads
as a broken build of llm.c. On MN5 `libcupti.so.12` lives in
`$COMSHU_SDK/cuda/extras/CUPTI/lib64`, which nothing in `common.sh` puts on
`LD_LIBRARY_PATH`. Append it before selecting `-p kernel`:

```bash
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$COMSHU_SDK/cuda/extras/CUPTI/lib64"
```

Separately, and the reason this section existed before the failure:
`-p kernel` comes from `gpu-backends/cupti/libcupti_trace.so`, which is a
different subtree and, on MN5, a different **build directory**
(`build-cupti/`). `COMSHU_PROVIDER_PATH` takes a colon-separated list; a
selection naming `kernel` without that directory on the path makes `comshu run`
**exit 1** rather than capture nothing, which is the good failure mode but is
still a stopped job. The kernel arm here is written so that its failure does not
take the study with it.

## 4. NCCL_DEBUG=INFO is the only oracle below the NCCL API, and it belongs on the bare run

Nothing in a capture records which algorithm or protocol NCCL chose, or which
connections went over the fabric rather than NVLink (04_llmc's `QUESTIONS.md`
Q1). `NCCL_DEBUG=INFO` with `NCCL_DEBUG_SUBSYS=INIT,GRAPH,TUNING` prints one
line per connection — `via P2P/direct pointer` against `via NET/IB/<hca>` — and
one per collective with its size, algorithm and protocol.

Put it on the **bare** arm, not on a captured one: it is the independent oracle
the assertions are judged against, and an oracle that only exists inside the
thing being judged is not one. The captured arms run `NCCL_DEBUG=WARN` so the
comparison is not perturbed by NCCL's own logging cost.

## 5. Everything from 02_lammps round 2's notes about the harness applies here too

The runner in this directory is derived from that one and carries the same three
fixes, each of which cost a queue slot there:

* **`a / b` on two integers is FLOAT division in DuckDB.** `rank / 4 = rank / 4`
  compares 3.5 against 3.75. Use `//`. Every node-membership clause in
  `asserts/` uses it.
* **`params` is spliced in as a leading CTE**, so an assertion opening with its
  own `WITH` is a Parser Error. The runner turns the file's leading `WITH` into
  a comma.
* **`sum()` over zero rows is NULL**, so a ratio assertion passes on an empty
  table. Every assertion here that reaches its evidence through a join carries
  an explicit empty-table clause.

## 6. Captures go to scratch

`gpfs_home` for `etur02` is at 78.6 of 80 GB. A four-node `-p nccl,ib` capture
is not what the last gigabyte is for. This case writes to
`/gpfs/scratch/etur02/koc296797-apps/`.

## 7. Version pins

| thing | value |
|---|---|
| llm.c | upstream `f1e2ace`, built at `$HOME/llmc/train_gpt2cu` by 04_llmc |
| model | gpt2-tiny, `-e d6`: L=6, C=384, Vp=50304, T=1024, 30,357,504 parameters |
| MPI | HPC-X's, from `nvidia-hpc-sdk/25.7` (`comshu_load_runtime`) |
| NCCL | `$NCCL_HOME` under the newest SDK that `common.sh` resolves |
| comshu | `~/proj/comshu/build-apps06`, private to this round, checkout `a0b242e` |
| job | 45330280 |
