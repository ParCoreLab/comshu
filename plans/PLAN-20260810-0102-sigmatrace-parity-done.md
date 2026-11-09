# PLAN — parity with SigmaTrace on an IBGDA run

- **Created:** 2026-08-10 01:02 +03
- **Status:** TODO — designed; step 3's assertion written, nothing run yet.

## du04 access, since it is not obvious

`ssh a100` is configured with `ProxyJump university-base`, and **that bare name
does not resolve** — MagicDNS is not answering on simurgh-base. The jump has to
be spelled with the tailscale address:

```bash
ssh -J mktips@100.64.0.8 missa18@172.20.31.90     # university-base -> du04
```

Confirmed 2026-08-10: `du04`, 2x A100-SXM4-40GB, **`PeerMappingOverride=1`** —
the capability whose absence makes all four IBGDA cases SKIP on MN5.

`~/proj/pcomm` on du04 is a **partial, pre-suite deployment** (no `tests/`, no
git). It needs the current tree before any of this runs; that sync is the first
practical task, not part of the design.
- **Related:** `PLAN-20260803-2253-ibgda-device-initiated-capture-done.md`
  (the doorbell hijack this validates); `tests/hpc/cases/12_ibgda_doorbell`
  (the capture side already exists).

## The question

Does pcomm see every event SigmaTrace sees, on a GPU-initiated run? SigmaTrace
is the predecessor and the only independent implementation, so it is the only
external check available on the DEVX path.

**Timings are not comparable and do not need to be.** Both tools record a
doorbell's `prod_index` — SigmaTrace in `DoorbellRingData/Control`, pcomm in
`Mlx5Doorbell.prod_idx` and `Mlx5DeviceSend.wqe_idx`. That is a **per-QP
monotonic sequence number, not a clock**. A doorbell at index N on a given queue
pair is the same logical event in every run of the same workload, whenever it
happened. The whole comparison can therefore be built on sequence identity and
never touch a timestamp.

## Two runs, not one

Both tools are `LD_PRELOAD` interposers on the same `mlx5dv_*` entry points, and
pcomm's own `smart_dlsym` throws on a duplicate symbol registration. Co-residency
is a research question, not a test fixture. So: **two separate runs, same binary,
same node, same parameters** — and the cost of that choice is that run-to-run
variation becomes a confounder, which step 1 exists to bound.

Same binary matters. Case 12 builds `nvshmem_putget_sm80` from
`tests/hpc/cases/04_nvshmem/putget.cu` with fixed `ELEMS`/`ITERS`; SigmaTrace has
its own apps under `Apps/`, and using them would make the workload a second
variable. Build once, run under each tool.

## MEASURED 2026-08-10 — step 1 ran, and it inverted the design

Two pcomm runs of case 12 on du04, identical parameters, canonical fingerprint:

```
                 run A     run B
device_sends      1309      1309     identical
opcode histogram  same      same     atomic_masked_cs 59, rdma_write 1250
total_bytes     160000    160000     identical
wqe_bag       109d403a  109d403a     IDENTICAL
umem_bag      d9396ba8  d9396ba8     identical
doorbells          168       162     DIFFER
doorbell_bag  9dd22446  c7e18a66     DIFFER
```

**The work is deterministic; the notifications are not.** The same 1309 WQEs —
same slots, same opcodes, same lengths — were announced by a different number of
doorbells. NVSHMEM batches warp-combined writes and rings once per batch, and
where a batch ends moves with timing. The risk this plan listed last ("each tool
perturbs timing, which can change how many doorbells ring") is not a risk, it is
the measured behaviour, and it shows up between two runs of the *same* tool.

**This inverts what the plan said above.** "Doorbell identity and sequence is the
parity claim" is wrong: the doorbell count is the one thing that does not
reproduce, so it cannot carry a claim. The decoded WQE stream is the stable
surface.

And that creates the tension to design around: the surface that *is* repeatable
(the WQEs) is exactly where SigmaTrace is expected to disagree, because of the
AV-segment layout difference below. So the comparison may lean on neither
doorbell counts (unstable) nor decoded fields taken at face value (contested).

**What survives as a sound parity claim**, all three measured deterministic
here: WQE count, total bytes, and the opcode histogram. Plus the set of
announced slots — with doorbell counts compared only as "both tools saw rings
covering the same work", never as equality.

Corollary for the fingerprint in step 2: build it on the per-QP **WQE**
sequence, not the per-QP doorbell sequence. `wqe_bag` was byte-identical across
runs; `doorbell_bag` was not.

Both runs passed all 14 case-12 assertions, which also means the
trailing-doorbell rule in `doorbell_sequence_complete` holds across differently
batched runs rather than only the one it was derived from.

## Step 1 — self-repeatability, before any cross-tool claim

**Run pcomm twice and require the canonical fingerprint (below) to be
byte-identical.** Only then is a pcomm-vs-SigmaTrace difference attributable to
the tools rather than to the run.

This is the gate the suite already applies to goldens — "a golden is only
committed after the same query yields byte-identical output across two
independent runs" — and it matters more here, because a cross-tool mismatch with
no repeatability baseline is uninterpretable in both directions.

Do the same for SigmaTrace. If either tool is not self-repeatable on this
workload, the parity test as designed cannot run, and the finding is that
instead.

## Step 2 — the canonical fingerprint

Everything run-specific is projected away. What varies between runs and must not
appear: timestamps, virtual addresses, `qp_num`, `mem_id`, UAR/context pointers,
`wr_id`. What is stable and carries the meaning: sequence indices, opcodes,
lengths, sizes, and counts.

**Doorbells — the primary surface.** Per queue pair, build the ordered sequence
of `(prod_idx, opcode, length)`, then compare the **multiset of per-QP
sequences** across tools. Keying on a QP ordinal was rejected: it assumes both
runs create queue pairs in the same order, which nothing guarantees. A bag of
sequences is invariant to both QP naming and QP ordering, and still detects a
single missing or extra doorbell anywhere.

**Registrations and UARs.** Multiset of `(size, access)` for umem; counts and
`flags` multiset for UARs; open/close counts must balance per class.

**Scalars, as a fast triage line:** doorbell count, distinct QP count, opcode
histogram, total bytes.

## Step 3 — `prod_idx` contiguity, on each capture independently

Per `(rank, qp_num)`, `prod_idx` must form a contiguous run. A gap is a doorbell
that tool missed.

This needs no second tool at all, it is strictly stronger than a count
comparison because it *localises* the miss, and it should be an assertion in
case 12 regardless of whether the SigmaTrace comparison ever runs. **Build this
first** — it is most of the value for a fraction of the work.

## Step 4 — compare, and report a coverage matrix rather than a verdict

The assertion is **directional, not equality**: for every event class SigmaTrace
records, pcomm must record at least the same multiset. Extra records on our side
are a superset and expected — we emit `Mlx5DevxQp`, `Mlx5DevxQpConnect` and
`Mlx5DevxCmd`, which have no SigmaTrace counterpart, and SigmaTrace splits
doorbells into `Data`/`Control` where we carry one type distinguished by opcode.

Output is a table with one row per event class and four columns: *both*,
*pcomm only*, *SigmaTrace only*, *disagreed*. A single PASS/FAIL would discard
exactly the information the test exists to produce.

The mapping, read from `Profiler/Events/Mlx5DvEvents.hpp`:

| SigmaTrace | pcomm |
|---|---|
| `Mlx5dvMemoryRegistration` | `Mlx5Umem` (200) |
| `Mlx5dvMemoryDereg` | `EntityClose` opener=200 |
| `Mlx5dvUarAlloc` | `Mlx5Uar` (202) |
| `Mlx5dvUarFree` | `EntityClose` opener=202 |
| `DoorbellRingData` | `Mlx5Doorbell` (212) + `Mlx5DeviceSend` (214), data opcodes |
| `DoorbellRingControl` | `Mlx5DeviceSend`, non-data opcodes |

## **SigmaTrace is not ground truth for decoded fields**

This is the most important constraint in the plan and it is already established
by measurement, not suspected.

`things-to-circle-back-to` and the IBGDA plan record that SigmaTrace's WQE
decoder **assumes a full 64-byte AV segment and pointer data segments**, while
NVSHMEM 3.3.24 on du04 emits a **half AV (`ds == 4`) and inlines warp-combined
payloads** (`MLX5_INLINE_SEG`). pcomm's decoder was written from the NVSHMEM
source for exactly that reason.

So the two tools are expected to **agree on which doorbells rang and disagree on
what the WQEs said**. That is not a bug to chase during this test:

- **Doorbell identity and sequence is the parity claim.** `prod_idx` comes off
  the doorbell itself, needs no WQE parsing, and is where agreement is
  meaningful.
- **`opcode` and `length` are the diagnostic, not the claim.** A disagreement
  there localises to WQE layout, and case 12's data-plane `VERIFY OK` is
  independent evidence for our reading.

Design the comparison so a decode disagreement cannot fail the doorbell-identity
check. If they are conflated, the test will report "no parity" over a difference
we already understand and have already resolved in our favour.

## RESULT 2026-08-10 — the comparison ran

Both tools, same binary, same `1024 20`, same no-dmabuf configuration, separate
runs. `Tools/sigma_dump.cpp` reads SigmaTrace's event files and emits the same
TSV shape the pcomm side does, so neither tool's analysis code is in the loop.

| | SigmaTrace | pcomm (run C) |
|---|---|---|
| queue pairs | 4 | 4 |
| doorbells | 167 | 173 |
| opcode 0x14 (atomic) | 60 | 59 |
| opcode 0x08 (rdma_write) | **107** | **1258** |
| total bytes | **172,472,576,384** | **161,024** |

**Where they agree — and it is the part that matters for capture completeness.**
Same queue-pair count. Doorbell totals within 6 of each other, which is *inside*
the run-to-run variance pcomm showed against itself (168 vs 162 in step 1), so
these are indistinguishable at this precision. Atomic doorbells agree at 60 vs
59. Both tools saw the same control plane and rang-doorbell stream.

**Where they diverge, with an independent oracle to say who is right.** The
workload moves 1024 ints x 4 B x 20 iters x 2 PEs = **163,840 bytes**. pcomm
reports 161,024 — 98.3% of nominal, a 2,816 B shortfall consistent with capture
starting after the hijack installs. SigmaTrace reports **172 GB**, six orders of
magnitude out. That is not a measurement difference, it is a misparse.

Two distinct causes, both predicted:

1. **Batched WQEs are not decoded.** 107 data doorbells against pcomm's 1258
   decoded sends. One doorbell can announce many WQEs — measured strides of 64
   and 67 slots in step 3 — and SigmaTrace records one event per doorbell rather
   than walking the batch. The atomics match precisely *because* they are one
   WQE per doorbell; the RDMA writes are the batched ones.
2. **The length field is read from the wrong offset.** This is the AV-segment
   layout difference this plan flagged before any code ran: SigmaTrace assumes a
   full 64-byte AV segment and pointer data segments, NVSHMEM 3.3.24 here emits
   a half AV (`ds == 4`) and inlines warp-combined payloads. 172 GB is what
   reading a length out of an inlined payload looks like.

**So the parity claim holds where it was designed to hold and fails where the
plan said it would.** The doorbell stream — the layout-independent surface — is
in agreement. The decoded fields are not, and the workload's own parameters
adjudicate: pcomm is within 2% of a number derived from the command line, and
SigmaTrace is not close to it.

This is why the plan refused to treat SigmaTrace as ground truth for decoded
fields. Had the comparison been built on `opcode`/`length` equality, it would
have reported pcomm as wrong.

## RESOLVED 2026-08-10 — SigmaTrace HEAD runs on du04

The crash was **not** abseil skew and not a build problem. It was SigmaTrace
being blind to how NVSHMEM registers memory in the configuration we were running
— and **pcomm diagnosed it**, which is worth recording on its own.

Our capture answers the question directly:

```
rank,ticks,umem,addr,size,umem_id,access,dmabuf_fd,device,binding,entry
0,0,2,0x0,8,2,0x1,84,0,by_dmabuf,reg_ex
```

Every umem on that run is `binding = by_dmabuf`, `entry = reg_ex`, `addr = 0x0`.
Against that:

- SigmaTrace hooks `mlx5dv::devx_umem_reg_tag` and **has no `umem_reg_ex` hook
  at all** (grep across `Profiler/` and `Mlx5dvTrace/` finds none).
- Its GPU discovery is `cudaPointerGetAttributes(addr)` — and under dmabuf the
  address *is* `0x0`, so it could not succeed even if the door were hooked.

The failure chain, end to end:

1. NVSHMEM registers the send queue via `devx_umem_reg_ex` with a dmabuf fd →
   SigmaTrace never sees the registration.
2. `MLX5_CMD_OP_CREATE_QP` looks the umem up, does not find it, and
   **returns early without registering the queue pair** — the code even says
   `// TODO: this is probably wrong, UMEM reg GPU discovery can fail`.
3. The GPU rings a doorbell for that queue pair; `CUDAHijack.cpp:394` does
   `m_mlx5_qp_info.at(dbr_qpn)` under the comment *"Should be guaranteed to
   exist"*, and `std::out_of_range` aborts the process.

This is exactly the class of blindness pcomm hit when UCX opened `ibv_context`s
through an un-interposed door: an object created somewhere the tracer does not
watch, then looked up unconditionally.

### The working configuration

**`NVSHMEM_IB_DISABLE_DMABUF=1`.** With dmabuf off and `nvidia_peermem` loaded
(it is, on du04), NVSHMEM registers GPU memory *by address*,
`cudaPointerGetAttributes` succeeds, the queue pair is registered, and the
doorbell handler finds it.

This is not a workaround invented here — `tests/hpc/cases/11_ibdevx_peermem`
already says in its own comments that **ibdevx with DMABUF disabled is
SigmaTrace's published artifact configuration**, and case 10 is the IBGDA
variant of it. The answer was in our own suite.

```bash
# du04.  ssh -J mktips@100.64.0.8 missa18@172.20.31.90
export PATH=/usr/local/cuda-13.0/bin:$PATH
export NVSHMEM_HOME=/opt/nvidia/hpc_sdk/Linux_x86_64/25.9/comm_libs/13.0/nvshmem
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$LD_LIBRARY_PATH"
export NVSHMEM_BOOTSTRAP=MPI NVSHMEM_DISABLE_P2P=1 \
       NVSHMEM_REMOTE_TRANSPORT=ibgda NVSHMEM_IB_ENABLE_IBGDA=1 \
       NVSHMEM_IBGDA_NIC_HANDLER=gpu \
       NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem \
       NVSHMEM_IB_DISABLE_DMABUF=1          # <-- the one that matters

mpirun -np 2 -x PATH -x LD_LIBRARY_PATH -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P \
  -x NVSHMEM_REMOTE_TRANSPORT -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_IBGDA_NIC_HANDLER \
  -x NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE -x NVSHMEM_IB_DISABLE_DMABUF \
  ~/SigmaTrace/build/target/Profiler --enable-doorbell-hijack --load-cupti=0 \
  -f -o ~/pcomm-parity/sigma  ~/nvshmem_putget_sm80 1024 20
```

Result: `nvshmem put-ring OK: 2 PEs, 1024 ints, 20 iters`,
`doorbell_submitted: 20` per rank, `Exited Normally!`, 33 non-empty event files.

Two invocation traps, both silent:

- **Do not pass `--` before the program.** With it the app received
  `0 ints, 1024 iters` instead of `1024 ints, 20 iters` — a *different workload*,
  which would have made any comparison meaningless while looking fine.
- `--load-cupti=0` is not required for the fix but removes a moving part.

### Build recipe on du04

```bash
pip3 install --user 'cmake>=3.31'      # ships 3.23.1; SigmaTrace needs >= 3.31
sudo apt-get install -y libabsl-dev    # only if not building abseil from source
git submodule update --init --recursive Imports/abseil-cpp   # pinned 20260107.0
cmake -B build -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.0/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=g++-12 -DCMAKE_CUDA_ARCHITECTURES=80 \
  -DBUILD_ABSEIL=ON \
  -DSIGMA_ENABLE_PERFETTO=OFF -DSIGMA_ENABLE_UCX=OFF \
  -DSIGMA_ENABLE_LIBFABRIC=OFF -DSIGMA_ENABLE_NVBIT=OFF
cmake --build build -j8
```

- `clang-20` is present and satisfies "GCC >= 13 or Clang >= 20"; the box has
  only gcc-11/12, so without it a compiler build would have been needed.
- **Watch the configure summary for `GPU backend`.** Without
  `CMAKE_CUDA_COMPILER` it reports `NONE` and silently builds a SigmaTrace with
  no doorbell hijack — the one component this comparison needs.
- `BUILD_ABSEIL=ON` is required to use the pinned submodule; without it the
  build silently takes Ubuntu's `20210324`, five years older than the pin.

### Consequence for the comparison

Runs A and B were captured **with dmabuf** and SigmaTrace needs it **off**, so
they are not comparable as they stand. The parity run must repeat pcomm in this
same configuration — which is case 10's, already in the suite.

That is also a finding in its own right: **pcomm traces this workload in both
memory-registration modes; SigmaTrace traces only one.**

## Superseded — the original blocked note

It **builds** on du04, which was the part expected to be hard:

- `clang-20` is installed and satisfies its "GCC >= 13 or Clang >= 20"; the box
  has only gcc-11/12, so without clang this needed a compiler build.
- CMake 3.23.1 is below its >= 3.31; `pip3 install --user 'cmake>=3.31'` gives
  3.31.6.
- abseil from `apt install libabsl-dev`; Perfetto/UCX/libfabric/NVBit disabled,
  since we read the event files directly rather than through its exporter.
- The first configure reported **GPU backend: NONE** — `nvcc` is not on PATH and
  CUDA lives at `/usr/local/cuda-13.0`. That would have silently produced a
  SigmaTrace with no doorbell hijack, i.e. the one component this comparison
  needs. Reconfigured with CUDA 13.0.48, host compiler g++-12, sm_80.

It then **runs, loads all three traces, and aborts before recording anything**:

```
[Profiler] loaded Ibverbs Trace
[Profiler] loaded mlx5dv Trace
[Profiler] Loaded Doorbell Hijack Trace
terminate called after throwing an instance of 'std::out_of_range'
  what():  absl::container_internal::raw_hash_map<>::at
[Profiler] doorbell_submitted: 0
```

**Not caused by this build.** `~/pcomm-ibgda/sigma2.log` from 2026-08-04 ends in
the same exit code 6, and the only other historical run
(`sigma_run.log`) was 1 PE with `doorbell_submitted: 0` — a single PE has no
remote peer, so it exercised nothing. There is no usable historical baseline.

Still crashes with `--load-cupti=0`, so it is in the ibverbs/mlx5dv/doorbell
path, not the CUPTI handler.

Two candidate causes, in order:

1. **abseil version skew.** SigmaTrace pins `abseil-cpp 20260107.0`; Ubuntu
   22.04's package is `20210324`, five years older. The next thing to try is
   cloning the pinned tag into `Imports/abseil-cpp` and rebuilding — mechanical,
   and the highest-probability fix.
2. **A genuine missing-key lookup.** `raw_hash_map::at` throws when a key was
   never inserted, and SigmaTrace's own reader carries a
   `WARNING: Unknown context_addr` for exactly that shape. If so it is the same
   class of blindness pcomm hit when UCX opened `ibv_context`s through a door
   the provider did not interpose — a context created somewhere SigmaTrace does
   not see, then looked up unconditionally.

If (2) is the cause it is a *finding about NVSHMEM 3.3.24 on this box*, not a
setup problem, and it means the parity comparison cannot be run against this
version of SigmaTrace without fixing it first.

Localising it further needs a backtrace from the **traced child**, not the
launcher: `Profiler` spawns the application with `LD_PRELOAD`, so gdb on
`Profiler` traces the wrong process. Reproducing the preload by hand
(`LD_PRELOAD=libProfilerShim.so:providers/libibverbs_trace.so`) segfaults
instead, so the launcher does setup beyond the preload string and the honest
route is a gdb wrapper the launcher itself execs.

## Step 5 — reading SigmaTrace's logs

SigmaTrace's `ProfilerEventReader` produces a comm-graph JSON and a Perfetto-ish
trace — neither is a raw event dump. Its `Events/EventReader.hpp` is the reading
primitive, and `Events/Tests/reader_visitor.cpp` is a worked example.

Write a small dumper against that header emitting the same fingerprint TSV
pcomm's side emits, so the comparison is TSV-vs-TSV and neither tool's analysis
code is in the loop. Do not compare via the comm-graph JSON: it is a derived,
opinionated view, and a difference in it would not distinguish "missed an event"
from "aggregated it differently".

## Risks

- **The workload may not be deterministic enough**, in which case step 1 fails
  and this plan stops there. That is a real possible outcome and a publishable
  one; it must not be worked around by loosening the fingerprint until the test
  passes.
- **du04 is the only host.** All four IBGDA cases SKIP on MN5 for want of
  `PeerMappingOverride=1` (confirmed on the 2026-08-10 board, job 44451619), so
  none of this is reachable from the cluster the rest of the suite runs on.
- **Building SigmaTrace** is unscoped here and may be the largest single cost.
- **Interposer interference**: even in separate runs, each tool perturbs timing
  (the hijack costs ~+68% wall), which can change how NVSHMEM batches warp
  writes and therefore *how many doorbells ring*. If doorbell counts differ but
  each is internally contiguous, suspect this before suspecting a missed event —
  it is a difference in the observed program, not in the observation.

## Explicitly out of scope

- Running both tools in one process.
- Timing comparison of any kind.
- Parity on the verbs path — the ib_* records have their own suite coverage, and
  the DEVX path is where we have no independent check.
- Fixing SigmaTrace's WQE decoder.
