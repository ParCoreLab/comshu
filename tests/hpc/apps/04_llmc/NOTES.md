# 04_llmc — environment traps, so the next agent does not pay for them again

Application: [karpathy/llm.c](https://github.com/karpathy/llm.c), `train_gpt2.cu`,
upstream `f1e2ace`. Machines: du04 (4x A100, no queue) then MN5 (2 nodes x 4 H100).

---

## llm.c does not compile under CUDA 13, and this is not a pcomm problem

`llmc/cuda_utils.cuh:220` calls

```c
cudaMemAdvise(*out, bytes, cudaMemAdviseSetPreferredLocation, cudaCpuDeviceId)
```

CUDA 13 changed that fourth parameter from an `int` device ordinal to a
`cudaMemLocation` struct, so nvcc 13.0 stops with

```
llmc/cuda_utils.cuh(220): error: no suitable constructor exists to convert from "int" to "cudaMemLocation"
```

**du04 has only CUDA 13** — `/usr/local/cuda`, `/usr/local/cuda-13.0` and the HPC
SDK 25.9 tree are all 13.0, there is no 12.x anywhere on the box — so this is
unavoidable there. `llmc-cuda13-memadvise.patch` in this directory is the
minimal fix: a `#if CUDART_VERSION >= 13000` arm that builds the struct. It is a
no-op on MN5, where nvcc is 12.9.

Worth recording for its own sake: **the call site is llm.c's managed-memory
fallback.** `cudaMallocConditionallyManaged` drops to `cudaMallocManaged` when a
`cudaMalloc` for the AdamW optimizer state OOMs, which is exactly the
`plans/todos.txt` item-2 territory the brief asks about. It never fired in these
runs (gpt2-tiny is 30 M parameters), so this case has nothing to say about
managed-memory attribution beyond noting that llm.c *has* such a path — plus one
that always fires, `cudaMallocManaged(&unified_buffer, sizeof(float))`
(`zero.cuh:455`), four bytes of managed memory used as an NCCL reduction buffer.
Under `-p gpumem` that four-byte allocation would be the smallest possible test
of `mem_object.device` on migrating memory. Nobody has run it.

The general lesson the brief asks for: **a third-party application is pinned to a
toolchain in ways our own workloads never are.** Every `tests/hpc/*.cu` in this
repo is written against whatever nvcc is present. llm.c is written against
CUDA 12, and no amount of care with pcomm's build makes that go away.

## Building the nccl provider on a fresh host needs two paths spelled out

```
CMake Error at providers/nccl/CMakeLists.txt:10 (find_path):
  Could not find CUDA_RT_INCLUDE_DIR using the following files: cuda_runtime_api.h
```

`find_path(CUDA_RT_INCLUDE_DIR cuda_runtime_api.h REQUIRED)` does not search
`/usr/local/cuda/include`, which is where the header is on du04. Configure with

```bash
cmake -B build -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_NCCL=ON \
      -DCUDA_RT_INCLUDE_DIR=/usr/local/cuda/include -DNCCL_INCLUDE_DIR=/usr/include
```

Not a bug — the provider is deliberately not linked against libnccl and has no
CUDA target to inherit include dirs from — but it is a five-minute stop the
first time, and `-DPCOMM_PROVIDER_NCCL=ON` alone does not work anywhere the CUDA
toolkit is not in CMake's default prefix path.

## du04: nvcc, cublas and MPI are in three different places

| what | where |
|---|---|
| `nvcc`, `nvdisasm` | `/opt/nvidia/hpc_sdk/Linux_x86_64/25.9/cuda/13.0/bin` |
| `cublas_v2.h`, `libcublas.so` | `.../25.9/math_libs/13.0/{include,lib64}` — **not** beside nvcc |
| OpenMPI 4.1.4 | `/usr/local/{bin,lib,include}` — **not** `/usr/lib/x86_64-linux-gnu/openmpi` |
| NCCL 2.28.9 | `/usr/include/nccl.h`, `/usr/lib/x86_64-linux-gnu/libnccl.so.2` (dpkg) |

`/usr/local/cuda/include` has no `cublas_v2.h`, so llm.c's `make` fails on it
even after the CUDA-13 patch. `CPATH` and `LIBRARY_PATH` pointing at `math_libs`
are enough; `run.sh` sets them. llm.c's Makefile hardcodes
`OPENMPI_DIR ?= /usr/lib/x86_64-linux-gnu/openmpi`, so pass
`OPENMPI_DIR=/usr/local`, and pass `GPU_COMPUTE_CAPABILITY=80` because its
autodetect shells out to a tool that is not there.

`libnccl.so.2` on du04 statically links cudart, so a CUDA-12-built NCCL and a
CUDA-13-built application coexist without a runtime conflict. Checked with
`ldd`, not assumed.

## MN5: llm.c's Makefile cannot detect NCCL, so do not use it

The Makefile gates `-DMULTI_GPU` on `dpkg -l | grep -q nccl`. MN5 has no dpkg,
so `make` silently produces a **single-GPU binary that calls no NCCL at all** —
which under `pcomm run -p nccl` looks exactly like a provider that failed to
hook. Invoke nvcc directly instead, as `job.sbatch` does. This is the
"four different nothings" case from `apps/README.md`, and it is the one that
would have been hardest to diagnose from a summary.

`common.sh`'s `$PCOMM_SDK` resolves to the **newest** SDK (26.5) while
`pcomm_load_build` puts 25.7's nvcc on PATH. That mix is what cases 02 and 20
already use (26.5's NCCL 2.29.3, 25.7's nvcc 12.9 and mpirun), and llm.c builds
and runs against it. Take cublas from `$PCOMM_SDK/math_libs/12.9`, matching the
nvcc, not from the unversioned `math_libs/include` symlink.

`-lnvidia-ml` is unconditional in llm.c's Makefile and there is no libnvidia-ml
on a login node; link against `$PCOMM_SDK/cuda/lib64/stubs` and keep that
directory **out** of `LD_LIBRARY_PATH` at run time.

## The dataset constrains the batch size, in a way that looks like a bug

tinyshakespeare's validation split is exactly **32,768 tokens**, and
`dataloader.h:184` asserts `shard_ntok >= num_processes*B*T + 1` on every shard.
At 8 ranks, llm.c's default `-b 4` needs 32,769 and the job dies in the loader
before NCCL is ever initialised. `-b 2` clears it. Batch size changes no
collective in this workload — the gradient tensors are model-shaped, not
batch-shaped — so both machines run `-b 2` and the derivation is identical.

Data must be tokenized off-cluster (`dev/data/tinyshakespeare.py`, needs
`tiktoken` + `numpy`; its `from transformers import AutoTokenizer` is only for
the llama-3 path and can be commented out to avoid pulling in torch) and
rsync'd. MN5 has no outbound network.

## `sizeof(float)` as an element count

See `asserts/scalar_reduction_count_is_sizeof_float.sql`. Four call sites pass
`sizeof(float)` where `ncclAllReduce` wants a count, so every scalar reduction
in llm.c is a four-element reduction, and one of them writes 16 bytes into a
4-byte `cudaMallocManaged` buffer. **The assertion expects 4 and passes.** Do not
"fix" it to 1 — that would be exactly the inversion `suite/README.md` warns
about, and the number 4 is `sizeof(float)`, an oracle from the C language rather
than from the capture.

## `nccl_p2p` is empty, not absent

`pcomm export --csv` writes a header-only CSV for every known record type, so
`nccl_p2p.csv` exists with zero rows and `sql_check.sh` loads it as an empty
table. That is what lets `no_point_to_point_and_two_streams.sql` *assert*
emptiness. Note the sharp edge: `-- requires: nccl_p2p` would report `[skip]`
for an empty table, which is why that assertion does not require it — and why an
assertion whose table is genuinely missing would fail to parse and read as a
pass. Case 20's `comm_hash_joins_the_ranks.sql` carries the same warning from
job 44724071.

## Things that cost nothing this time and might next time

- Nothing here uses NVBit, so du04's missing `nvdisasm` never came up. The
  `nccl` provider is a host-API interposer: it costs milliseconds, not the ~440 s
  case 05 spends lifting NCCL's module.
- `-p nccl,ib` composes in one process with no replay — both are hook providers.
  A `gpumem` pass would have forced replay and turned one execution into two,
  which would have destroyed the intent-to-wire causality this run exists for.

## NCCL's inter-node sends are a third zero-length, and that is correct

Job 45082117, stage 0, all eight ranks, `-p nccl,ib`:

    opcode          n      zero_len  min_len  max_len
    rdma_write      22558  0         64       75776
    rdma_write_imm  11558  11000     0        8192

`rdma_write_imm`'s payload IS the 32-bit immediate, so `length = 0` is right
there and nowhere else. An assertion demanding payload on every send produces
11 000 false positives per stage-0 rank. `suite/asserts/sends_have_payload.sql`
already excludes `%_imm` and says why (case 02 posts 1120 such sends per rank) —
**read it before writing an IB assertion of your own.** I did not, and paid for
it; see REPORT.md §5.

Two other guesses about the IB side that turned out wrong, recorded so the next
agent does not inherit them:

- **All ranks post on IB**, not just the ones owning a cross-node ring hop. NCCL
  builds several rings and gives every local GPU an inter-node connection, so
  the `07_nvshmem_multinode` asymmetry does not carry over to NCCL.
- **`-p nccl,ib` composes with no replay and no measurable cost.** Both are hook
  providers, so one process hosts both and the intent-to-wire causality survives.
  A `gpumem` pass in the same selection would have forced replay and turned one
  execution into two, which would have destroyed exactly that.
