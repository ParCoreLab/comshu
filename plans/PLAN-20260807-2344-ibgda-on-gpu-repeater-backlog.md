# PLAN — the on-GPU doorbell repeater: keep the fast path fast

> **Backlog 2026-11-09 (triage).** Only useful where the GPU can map a doorbell page (du04, PeerMappingOverride=1); MN5 runs the host doorbell. Not next work.

- **Created:** 2026-08-07 23:44 +03
- **Status:** TODO, and deliberately not started. This is Stage 3 of the
  IBGDA data-plane work; Stages 1-2 (the CPU repeater + WQE decode) shipped and
  reach parity with *working* SigmaTrace. This stage is a capability neither
  codebase currently has.
- **Parent:** `PLAN-20260807-1746-ibgda-data-plane-hijack-done` — read its
  "Stage 2 SHIPPED" close first; the numbers there are the whole justification
  for this plan, and the whole argument for not building it yet.
- **Prior art / design source:** `../SigmaTrace/Profiler/Modules/GPUMemoryHijack/`
  — `GPUMemoryRepeater.cuh` (the host+device shared structures, complete and
  worth copying) and `GPUMemoryRepeater.cu` (the kernel — **do not port it as
  working code**: `parse_and_record` is unreachable, `parse_and_record_oneshot`'s
  one call site is commented out, and the poll loop carries a bare
  `printf("UPDATING")`. The *design* is the reference; the *implementation* is
  not).

> **MEASURED 2026-09-06, and it changes the justification for one of the two
> paths.** On the HOST doorbell path (CPU-assisted IBGDA, MN5 job 45490908) the
> forward is FREE — decoy page plus repeater within ±3% of an uninstrumented
> run, i.e. inside noise — and the per-doorbell copy-out is the entire cost, up
> to 13% of bandwidth at 16 KiB. An on-GPU repeater removes the forward, so it
> would not move that number. This plan's premise still holds for the GPU hijack
> on du04, where the forward genuinely is GPU → host → BAR; it does not hold for
> the mode MN5 runs. Before building this, decide which path it is for, and note
> that the host path's cost has cheaper fixes: stop synchronising per doorbell,
> or read the send queue through a GDRCopy host mapping rather than a CUDA copy.
> **BOTH HALVES NOW MEASURED (du04, 3 reps, same harness).** On the GPU hijack
> path the forward is 16-45x — it costs 95-98% of throughput from 256 B to
> 128 KiB, and the decode is noise beside it. So this plan is not merely still
> valid for du04, it is strongly justified there and would recover 20-40x. What
> the two measurements together say is that the forward and the copy-out trade
> places between the paths, so build this for the GPU path and fix the copy-out
> for the host path; neither fix helps the other.
> See tests/hpc/probes/uar_doorbell_mode/README.md.

## Why this exists, in one paragraph

A GPU-initiated send is one GPU store to a NIC BAR page. The CPU repeater
(Stage 1-2) observes it by redirecting that store to host memory, waking a host
thread, and replaying it — which reintroduces the CPU onto the exact path IBGDA
exists to bypass. Measured on du04: **+68% wall for the pure forward, +86% with
the WQE copy-out**, and `wqes_lapped > 0` once the GPU outpaces the host. Both
costs come from the same root: a host round trip per doorbell, and a host thread
that cannot keep up with a GPU producer. The on-GPU repeater removes the root —
observe and forward *on the GPU* — so the fast path stays `GPU → BAR` and the
observer runs at GPU speed.

## What changes, mechanically

| | CPU repeater (shipped) | on-GPU repeater (this plan) |
|---|---|---|
| fake doorbell page | host pinned memory (`cuMemHostAlloc`) | **GPU HBM** (`cuMemAlloc`) |
| who polls it | a host thread in `UMWAIT`/`PAUSE` | a **persistent CUDA kernel** |
| how the doorbell is seen | GPU store → PCIe → host memory → wakeup | GPU store → HBM; kernel reads HBM |
| WQE inspection | `cuMemcpyDtoHAsync` out of the SQ, host parse | parsed **in place** in HBM, no copy |
| forward to NIC | host store to BAR | **GPU store** to BAR (native shape) |
| record delivery | direct `Recorder::record` on host thread | GPU pushes to a pinned ring; host drains lazily, off the critical path |

The forward is the load-bearing difference: it goes back to a single GPU→BAR
store, so the send's latency is (near) native and a timing measurement taken
under this repeater is about the application again, not the instrument.

## The design, from `GPUMemoryRepeater.cuh`

One pinned `GpuRepeaterChannel`, mapped into both host and device address
spaces, is the entire host↔device contract:

- **UAR table** (`GpuUarEntry{fake_addr, real_addr}[MAX_UARS]`, `num_uars`):
  host writes, GPU reads. `fake_addr` is the HBM page the app rings; `real_addr`
  is the NIC BAR device pointer the kernel forwards to.
- **QP table** (`GpuQpEntry{sq_base, qpn, log_wq_size, qp_type, gpu_id}[MAX_QPS]`,
  `num_qps`): host writes, GPU reads. This is the same `(qpn → sq_base, shape)`
  map Stage 2 already builds in `hijack::register_qp` — it moves into the channel.
- **Result ring** (`GpuWqeRecord result_ring[16384]`, `result_head` GPU
  producer, `result_tail` host consumer): GPU writes decoded WQEs, host drains.
  `GpuWqeRecord` already carries everything `Mlx5DeviceSend` needs
  (`raddr/laddr/rkey/lkey/byte_count/qpn/wqe_idx/opcode/ds`) plus a device
  `clock64()` timestamp.
- **`shutdown`**: host sets it, the kernel exits its poll loop.

The kernel (`gpu_doorbell_intercept_kernel`) is a persistent loop: for each UAR,
read `*fake_addr`; on change, decode `(qpn, prod_idx)`, look up the QP, parse the
delta WQEBBs straight out of `sq_base` in HBM, push a `GpuWqeRecord` per WQE into
the ring, then store the doorbell value to `real_addr`. **The WQE parse is the
one Stage 2 already got right** (`uar_hijack.hpp` — half AV segment, inline-flag
detection, front-based offsets); it must be ported to device code *from comshu's
version, not SigmaTrace's*, because SigmaTrace's assumes a full AV and pointer
data segments and is wrong on this NVSHMEM.

## The hard part: it steals an SM from the workload it measures

A persistent kernel occupies SMs the application wants. Left unbounded it
contends with the application's kernels and perturbs exactly what it is trying to
measure honestly. SigmaTrace's answer, which this plan adopts: run the repeater
in a **CUDA green context with a single SM**, so the profiler kernel is confined
to one SM and the scheduler keeps it off the application's. Porting that carving
— not a plain persistent kernel — is a first-class requirement, not a
refinement. Without it this trades the CPU repeater's host round trip for a
whole-GPU contention that is harder to reason about.

## Stages

### Stage 3.0 — the channel and a green context that does nothing
Allocate the `GpuRepeaterChannel` (pinned), create a one-SM green context, launch
an empty persistent kernel in it that only polls `shutdown`. Prove: the
application runs unperturbed beside it (byte-identical, and overhead inside the
noise of three clean runs — the same gate Stage 1 used). If a bare one-SM
persistent kernel already perturbs the workload, the whole approach is in
question and this is where that is cheap to learn.

### Stage 3.1 — forward only, no decode
Move the fake UAR page to HBM (`cuMemAlloc` instead of `cuMemHostAlloc`), fill the
channel's UAR table at the swap, and have the kernel forward `fake_addr →
real_addr` with the correct release ordering. No parsing, no ring. Gate: full
NVSHMEM run byte-identical, and **overhead materially below the CPU repeater's
+68%** — that delta is the entire reason to build this. If it is not below, stop:
the GPU forward is not buying what the plan promised.

### Stage 3.2 — decode on-device, drain on-host
Fill the QP table, port comshu's Stage-2 WQE parse into the kernel, push
`GpuWqeRecord`s to the result ring, and drain it on a host thread that translates
each into `Mlx5Doorbell`/`Mlx5DeviceSend` through the existing sinks. The records
are unchanged — only their *source* moves to the GPU. Gate: on the case where the
CPU repeater reported `wqes_lapped > 0`, this reports **zero** (or far less), and
the send count is complete against the workload's known pattern.

### Stage 3.3 — the ordering proof
`GpuWqeRecord` and the forward both cross the GPU→PCIe→NIC boundary, where store
ordering is not CPU ordering — the open question SigmaTrace flags and does not
resolve (`MemoryRepeater.hpp:59`). Whatever fence discipline the kernel uses
between "WQEs visible in the SQ" and "doorbell visible at the BAR" needs a written
justification against the mlx5 PRM and the CUDA memory model, not
`__threadfence()` because it ran. Getting this wrong corrupts the *application*,
not the trace — this stage is where that risk is discharged or the plan halts.

## Verification, and the honesty rule that gates the whole thing

Same as the parent: **du04 only** (`missa18@a100` via university-base). MN5 cannot
run IBGDA.

The load-bearing measurement is NOT on the loopback microbenchmark. Stages 1-2
were measured on a doorbell-bound put-ring where the doorbell rate is nearly the
whole workload — the worst case for the CPU repeater and therefore the case that
*most* flatters this one. Before 3.1's gate is trusted, the comparison has to be
re-run on **a workload with real GPU compute between sends**, where the doorbell
is a small fraction of the work. It is entirely possible that there the CPU
repeater's overhead is already in the noise and this whole stage is 770 lines of
CUDA for a problem the realistic workload does not have. The parent plan's own
rule, inherited verbatim: *if the CPU repeater is inside budget on the cases we
have, do not carry the GPU repeater — and say so here, with the number.*

## Risks

- **Green-context availability.** One-SM carving depends on the driver/arch
  supporting green contexts (CUDA 12.4+; check on du04's driver before building).
  If unavailable, the fallback is MPS or a plain persistent kernel with an
  explicit SM cap, each with worse isolation — and that weakens the timing claim
  the stage exists to make.
- **New CUDA in the provider.** The mlx5dv provider does not link CUDA today (it
  resolves everything at runtime). A device kernel means the provider — or a
  sidecar `.so` it dlopens — now compiles with `nvcc` and carries a `.cu`. That
  is a build-system change (the gpumem provider already shows the shape) and it
  must stay behind the `--enable-doorbell-hijack` gate so a CUDA-less host
  (MN5 core) still builds the passive provider.
- **The reference is broken.** Unlike Stages 1-2, there is no working
  implementation to port — SigmaTrace's kernel does not run. Every line is new,
  and the parse is the only piece with a trusted source (comshu's own Stage 2).
- **Perturbation of a different kind.** This does not eliminate perturbation, it
  changes its shape from host-round-trip latency to one-SM contention. A timing
  claim under it is still a claim about an instrumented system — better, not
  clean. Say so wherever a number from it is published.

---

## THE MEASUREMENT THIS PLAN WAS WAITING FOR — du04, 2026-08-27

The parent plan said not to build this blind: "*the honest next step is a
Stage-2 measurement on a workload with real compute (where the doorbell rate is
a fraction of the work) before committing to a persistent green-context
kernel.*" Here it is, plus a second axis the parent could not separate.

`tests/hpc/probes/ibgda_compute_ratio/` — the same put-ring with a FLOPs knob, so
`steps = 0` reproduces `04_nvshmem/putget.cu` exactly and everything to the right
walks the compute:communication ratio out. Median of 3, two PEs, 200 iterations,
1024 elements, `NVSHMEM_REMOTE_TRANSPORT=ibgda` + `NIC_HANDLER=gpu`.

Timed by the application's OWN CUDA events around the communication loop, not by
wall clock. **This is why the numbers look nothing like the published +68%/+86%:
those were wall clock over a run whose NVSHMEM startup was ~3.5 s of a ~3.5 s
run, so they measured the hijack diluted by a constant.** Same effect, undiluted:

| compute steps | bare ms | passive ms | forward-only ms | full hijack ms | fwd | hijack |
|---|---|---|---|---|---|---|
| 0 | 11.06 | 11.08 | 1224.0 | 1627.1 | **110.7x** | **147.1x** |
| 200 | 11.22 | 11.26 | 1267.0 | 1559.0 | 112.9x | 138.9x |
| 2000 | 12.72 | 12.73 | 1286.1 | 1597.9 | 101.1x | 125.6x |
| 20000 | 27.53 | 27.54 | 1299.0 | 1564.0 | 47.2x | 56.8x |

`passive` (`-p mlx5dv`, control plane, no hijack) is **free to three decimal
places at every point**, which is what makes the rest attributable to the
man-in-the-middle rather than to the hooks or the capture's I/O.

### Two findings, and they point the same way

**1. The cost is the FORWARD, not the decode.** Forward-only is 110.7x; adding
the WQE copy-out and parse takes it to 147.1x. So the whole per-doorbell
`cuMemcpyDtoHAsync` + `cuStreamSynchronize` — the obvious thing to optimise, and
the thing batching would address — accounts for **1.33x of a 147x problem**.
Batching the copies, or a cheaper parse, cannot recover an order of magnitude.
Only removing the host round trip can, and that is what this plan is.

That is the question this measurement existed to answer, and the answer is not
the one the cheaper fix would have wanted. `COMSHU_HIJACK_NO_DECODE` was added to
ask it — measurement only, deliberately not a launcher flag, since it produces
`Mlx5Doorbell` rows with no `Mlx5DeviceSend` beside them and that is exactly the
shape a real decode failure has.

**2. Compute does not rescue it, and the shape says why.** The hijack's absolute
cost is ~1.2-1.6 s across the whole ladder — it does not move when the
application's own work grows 2.5x. So the ratio falls only because the
denominator grows, and reaching a 2x overhead by this route needs the
application to do ~1.3 s of compute per 200 exchanges. The parent plan's hope
that "a real compute workload would dilute it" is true arithmetically and
useless in practice: the per-doorbell cost is ~144 us against ~1.3 us unhijacked,
and it is paid serially because the GPU stalls until the forward lands.

### Verdict

**Stage 3 is justified, and now on evidence rather than on the +86% from a
microbenchmark.** The CPU repeater is a correctness tool — it captures the data
plane faithfully (case 12: 15 assertions green, `VERIFY OK` byte-identical) and
must stay off by default, which it is. It is not, and cannot be made into, a
tool you leave on.

Two caveats that belong with the number rather than in a footnote:

- **This is still one workload.** 1024 elements of `nvshmem_int_p` is one
  doorbell per element per iteration — a high doorbell rate by construction. An
  application that batches its puts rings fewer doorbells and pays less. What
  the measurement establishes is the per-doorbell cost and where it comes from,
  not a universal slowdown factor.
- **The absolute ~1.3 s is suspiciously flat** across a 2.5x change in
  application work, which is consistent with a serialised per-doorbell latency
  and worth confirming against the repeater's own wake path before it is quoted
  as a hardware number. WAITPKG is present on du04, so it is not a PAUSE-spin
  artefact.

### One harness defect worth recording

The first run of this sweep reported the decode costing 5%, which was wrong.
`COMSHU_HIJACK_NO_DECODE` was exported into the harness's shell and passed with a
bare `-x`, so OpenMPI put it in the `hijack` runs too and both columns measured
the same configuration. **The capture is what caught it**: the hijack run emitted
no `214.pcs` at all, and a hijack that records no `Mlx5DeviceSend` has not
decoded anything. The harness now verifies the distinction from the capture after
every sweep rather than trusting the environment that was supposed to produce it.
