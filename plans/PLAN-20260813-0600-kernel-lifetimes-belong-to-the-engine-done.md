# PLAN — kernel lifetimes belong to the engine, and CUPTI cannot be taken for free

- **Created:** 2026-08-13 06:00 +03
- **Status:** SHIPPED — all seven steps, both sources, verified end to end on
  MN5. Every number below is from a run on MN5 (H100, CUDA 12.6) or du04 (A100,
  CUDA 13.0); job 44546506 is the at-scale cost arm, 44546852 the combined
  comm+compute acceptance run.

  **Both sources verified on real captures.** CUPTI: 1402 rows, 0 dropped, from
  `pcomm run -p cupti`. NVBit globaltimer: **502 spans, 0 incomplete**, from a
  gpumem capture with no CUPTI in the process at all — `530.pcs` present beside
  `700`/`520`, `source = 1`, domain 2, names and geometry intact
  (`_Z10spin_timedmPfPmS0_`, `1,1,1 x 128,1,1`), counts exact against the
  probe's 250/250/2 launches.

  **Verified on a real capture** (MN5 login node, H100, `pcomm run -p cupti`):
  1402 `gpu_kernel` rows, 0 dropped, kernel names resolving
  (`_Z10spin_timedmPfPmS0_` x700, `_Z10spin_plainmPf` x700, `_Z5stampPm` x2 —
  exactly the probe's launch counts), grid/block geometry correct, median
  duration 11,040 ns against the probe's own 11 us kernels. `_clock` fits BOTH
  domains through the same generic per-(rank, domain) machinery with no special
  case: domain 0 at 0.4999998 ns/tick (2 GHz TSC) and **domain 1 at 0.9999999**,
  i.e. exactly the 1.0 this plan predicted for CUPTI's timebase.
  `timeline_unanchored = 0`.
- **Follows** `PLAN-20260810-0505-object-tracking-is-not-a-provider-concern`. The
  argument is the same argument, and it is the reason this plan exists at all.
- **Supersedes** the "Kernel lifetimes" section of
  `PLAN-20260813-0300-timeline-panel`, which proposed a `GpuKernel` record at
  type 300 with two open questions. Both questions are now answered, and one of
  the answers changes the design.
- **Probe:** `tests/hpc/probes/kerneltime/` (5 parts, all runnable).

## The question, in the form the precedent already settled

`PLAN-20260810-0505` opens by measuring that the same NVSHMEM workload knows
what the application allocated in one capture and not the other, and concludes:
*"does this capture know what the application allocated" currently answers "only
if you ran NVBit". Paying [NVBit's ~435 s] to learn a buffer's name is
backwards.*

Kernel lifetimes are that sentence again with one noun changed. **"Does this
capture know when kernels ran" must not answer "only if you ran NVBit."** A
comm-only timeline can show that rank 17 went quiet for 400 µs and can never say
whether it was computing or blocked — and every question worth opening a
timeline for (overlap vs serialisation, straggler vs stall, is the comm hidden
behind compute) is a question about comm spans *against* kernel spans.

So: **yes, this belongs in the engine.** The rest of this plan is about the one
constraint that makes it harder than it sounds.

## What we do and do not have today

- **SigmaTrace has it**: `Events/Events.hpp` declares `CudaKernelLaunch = 300` /
  `CudaKernelExit`, emitted from the NVBit, Sanitizer and CUPTI sources alike.
- **pcomm does not**: no kernel record anywhere in `pcomm.fbs`; the type table
  jumps from `mlx5_*` (200–214) to `mem_object`/`gpu_access` (520/700).
- **But `id_range::cupti_lo = 300, cupti_hi = 399` is already reserved** in
  `core/ids.hpp`, and our NVBit backend already intercepts `cuLaunchKernel`,
  `cuLaunchKernelEx` and `cuLaunchCooperativeKernel` and resolves the mangled
  name — then throws the event away, using it only to decide whether to
  instrument.

## Measurement 1 — submission time is not execution time, and it is not close

The cheapest imaginable design is a host-side interposer on the launch APIs, in
the engine, timestamping with TSC. The engine already interposes `dlsym` and
`nvshmem_*`, so it is a small change. It is also wrong, and here is by how much.

`probe.cu` correlates CUPTI's `RUNTIME` record (the launch API call) to its
`CONCURRENT_KERNEL` record (the execution) by `correlationId` — same timebase,
so the difference is exactly the error a host-side stamp would carry.

| platform | regime | kernel duration | `kernel.start − launch_api.start` p50 | p99 |
|---|---|---|---|---|
| H100 | sync (launch, sync, launch) | 11.0 µs | 4.4 µs | 6.1 µs |
| H100 | **async (a real queue)** | 11.0 µs | **9.16 ms** | 12.2 ms |
| A100 | sync | 19.9 µs | 6.4 µs | 9.5 µs |
| A100 | **async** | 19.9 µs | **12.76 ms** | 21.8 ms |

In the async regime — which is what every real comm workload does — submission
lands **~800× the kernel's own duration** away from execution. A timeline drawn
from launch timestamps would not be slightly wrong, it would be fiction.

Submission time is still *a* real quantity (it shows launch-side stalls, and it
is what the NVBit callback can cheaply give). It is simply not the kernel's
lifetime, and must never be drawn as one.

## Measurement 2 — CUPTI is cheap, exact, and needs no clock work at all

`CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL`, buffers via
`cuptiActivityRegisterCallbacks`. Works **unprivileged** on both platforms — no
`NVreg_RestrictProfilingToAdminUsers` relief needed, because that gates the
PerfWorks counter APIs, not activity records. 0 dropped records in every run.

**And the timestamps are already our timestamps.** `cuptiGetTimestamp` sampled
beside every candidate host clock:

| platform | `cupti − CLOCK_REALTIME` | `cupti − CLOCK_MONOTONIC_RAW` |
|---|---|---|
| H100 | **−162 ns**, then −38 ns | +1.79e18 (unrelated) |
| A100 | **−260 ns**, then −29 ns | +1.77e18 (unrelated) |

CUPTI's timebase **is** `CLOCK_REALTIME` in nanoseconds, which is exactly what
`ClockAnchor.wall_ns` already records and what `timeline_span` already resolves
to. A CUPTI-sourced kernel record needs no calibration and no new machinery —
it drops straight onto the timeline built today.

### Cost, measured where it counts

The microbenchmarks report 9.4 % (H100) but they launch 11 µs kernels
back-to-back, which is a pathological launch rate and an upper bound, and they
ran on shared login nodes — du04 even reported *negative* overhead, which is
contention noise, not a speedup. So the number that decides "can this be on by
default" came from a dedicated 2-node × 4-GPU NCCL all-reduce over IB
(job 44546506):

| arm | what | wall |
|---|---|---|
| A | bare | 12.815 s |
| B | + CUPTI kernel timing | 12.192 s |
| C | + ibverbs provider (what we ship) | 11.840 s |
| D | **+ ibverbs + CUPTI kernel timing** | **11.917 s** |

**C → D is +0.65 %.** A → B is *negative*, which fixes the honest reading:
**run-to-run noise here is ~±5 %, and the cost of kernel timing is below it.**
The point estimate is 0.65 %; the defensible claim is "not measurable against
this workload's own variance."

Volume is a non-issue: 50 kernels/rank over 10.3 s, ~1050 kernels/s during the
active span, **208 bytes of CUPTI activity per kernel** → ~218 KB/s/rank. Real
kernel durations were p50 678 µs, p99 13.3 ms, GPU 99.7 % busy.

## Measurement 3 — the constraint that changes the design

**NVBit dlopens `libcupti.so.12` and takes the process's one CUPTI subscriber
slot.** This is invisible to static analysis — `nm libnvbit.a` reports *zero*
undefined cupti symbols and the only injection string in it is
`CUDA_INJECTION64_PATH` — and it is unambiguous under `LD_DEBUG=libs`.

CUPTI permits one subscriber per process. So CUPTI activity capture and
gpumem/NVBit **cannot coexist**, and which one loses depends on load order:

| chain | CUPTI | NVBit | process |
|---|---|---|---|
| NVBit-then-shim | wins (`SHIM on`) | **dies: `CUDA_ERROR_ALREADY_ACQUIRED`** | **exit 1** |
| shim-then-NVBit | soft-fails: `CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED` | works (802 launches) | exit 0 |

**The asymmetry is the whole design input.** One direction fails softly with a
detectable error code and everything else keeps working. The other direction
kills the run.

And it is not a toy-tool artifact. Arm E ran the **real
`libgpumem_trace.so`** against the real NCCL workload with the shim in the
chain, and gpumem wrote **zero `.pcs` files** — four rank directories created,
nothing inside them. Not a crash with a message: a silently empty capture. That
is the precise failure mode `pcomm_engine_interposer_live` exists to prevent for
`dlsym`, arriving again through a different door.

## Measurement 4 — the fallback, for when CUPTI is unavailable

If gpumem is loaded, kernel timing has to come from somewhere else. NVBit can
inject a prologue/epilogue reading `%globaltimer`.

| platform | globaltimer vs CLOCK_REALTIME | injected-pair cost |
|---|---|---|
| H100 | rate ratio 0.999986 → **−13.86 ppm drift** | **1.10 %**, 131 ns/kernel |
| A100 | rate ratio 1.000006 → **+5.73 ppm drift** | 6.71 %, 1597 ns/kernel (contended box) |

Two conclusions. It is **cheap** — cheaper than CUPTI, and trivial next to the
NVBit cost already being paid. And it is **its own clock domain**: the drift
differs in sign and magnitude *per GPU*, so a shared constant would pull ranks
apart (13.9 ppm over a 7 s capture is 97 µs of skew against 11 µs kernels). It
needs a per-GPU `ClockAnchor`, which is precisely the per-rank TSC fit `_clock`
already performs.

### Rejected: CUDA events as the universal mechanism

Tempting, because `cudaEventRecord` touches no CUPTI machinery and would work in
both worlds. Measured against CUPTI on identical launches, it is **worse on both
axes**:

- **48.3 % overhead** (vs CUPTI's 9.4 % on the same microbenchmark).
- **+3.7 µs systematic bias** on an 11 µs kernel — a 34 % overstatement, because
  the markers bracket a position in the *stream*, not the kernel's execution.
  `DELTA_START` was 16.4 µs. This is a bias, not noise: it does not average out.

## Design

**The engine owns one `gpu_kernel` record. The engine owns the CUPTI source.
gpumem feeds the same record from NVBit when it is loaded.** This is
"independent emitters, dedup by identity" — the pattern `PLAN-20260806-1329`
settled and `PLAN-20260810-0505` extended — with emitter identity as a *field*
rather than a second record type.

```
// engine range, beside mem_object 520 / entity_close 522 / node_identity 524
struct GpuKernel {          // type 530, Event
  temporal:Temporal;        // kind = Interval: a = start, b = end, EXECUTION
  name:uint32;              // interned demangled symbol
  correlation:uint64;       // launch-site id; joins submission to execution
  stream:uint64;            // CUstream — lanes within a device
  device:uint16;
  source:uint8;             // 0 = cupti, 1 = nvbit_globaltimer
  grid_x:uint32; grid_y:uint32; grid_z:uint32;
  block_x:uint16; block_y:uint16; block_z:uint16;
}
```

Submission time is kept as a **separate optional record**, not squeezed into
this one, because Measurement 1 says they are different quantities by four
orders of magnitude and a field named `launch_ticks` next to `a`/`b` would
invite exactly the conflation that makes the timeline lie.

### Clock domains, and why the timeline needs no new code

`_clock` (added with the timeline panel) fits `ticks → wall_ns` per
`(rank, domain)` from `ClockAnchor` rows. That generalises for free:

| source | domain | anchor | fit comes out as |
|---|---|---|---|
| host records (today) | 0 (TSC) | existing | ~0.4999872 ns/tick |
| CUPTI kernels | 1 (wall ns) | `local_ticks = cuptiGetTimestamp()`, `wall_ns = CLOCK_REALTIME` | **exactly 1.0** |
| globaltimer kernels | 2 + gpu ordinal | sampled per GPU at start/end | ~1.0 ± ppm |

So both sources land on the timeline through the machinery that already exists,
and `link_health.timeline_unanchored` already counts anything that does not.
**Adding `gpu_kernel` to the `timeline_span` union is the entire UI change.**

### The ordering rule, which is not a preference

**The engine must never enable CUPTI when gpumem is in the provider set.**
Getting this backwards does not degrade the capture, it empties it (Arm E).
Two mechanisms, and both should be used:

1. **The launcher knows.** `pcomm run` composes the LD_PRELOAD chain and has the
   provider list in hand; it can simply not ask for CUPTI when gpumem is
   selected. Deterministic, and the primary control.
2. **Enable CUPTI lazily, not in a constructor.** The shim used
   `__attribute__((constructor))`, which is what let it win and kill NVBit. If
   the engine instead enables CUPTI at first launch — after NVBit would have
   initialised — then NVBit always wins on its own and the engine always gets
   the *soft* error. Belt and braces, and it removes the ordering dependency
   even when the engine is preloaded by hand rather than by `pcomm run`.

`CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED` should then be reported as a
*fact about the run*, not swallowed — a `link_health` row saying kernel timing
came from NVBit rather than CUPTI, or from nowhere.

### gpumem feeds the engine, it does not own the range

`pcomm_engine_declare_provider` enforces record-id range disjointness, so gpumem
cannot emit into the engine's 500–599. It shouldn't want to: the record is the
engine's. gpumem hands rows over a new ABI entry point, exactly as
`pcomm_engine_note_qp` already does for queue-pair closes:

```c
void pcomm_engine_record_kernel(uint64_t t0, uint64_t t1, uint16_t domain,
                                uint32_t name, uint64_t correlation,
                                uint64_t stream, uint16_t device, uint8_t source,
                                const uint32_t grid[3], const uint16_t block[3]);
```

## Steps

1. `GpuKernel` record + schema + generated headers. (Schema breakage is fine —
   we are pre-alpha and the goldens will rehash anyway.)
2. Engine: CUPTI activity source, enabled **lazily**, with the
   `MULTIPLE_SUBSCRIBERS` path treated as "defer, and say so".
3. Engine: `ClockAnchor` for domain 1 at start and end of run. Two rows, same
   shape as the TSC pair.
4. `pcomm run`: do not request CUPTI when gpumem is selected.
5. `link_sql.hpp`: add `gpu_kernel` to `_tl_raw`; add a `link_health` row for
   "kernel timing unavailable / from fallback".
6. `Timeline.svelte`: add the lane. Per the panel's design it needs no other
   change — the union is the change.
7. **Only then** the NVBit globaltimer source, plus per-GPU anchors on domain
   2+. It is the harder half and the capture is useful without it.

## What building it changed about the design

Three things the measurement pass did not predict, all found by running it:

1. **Launch interposition does not arm the backend.** The plan assumed CUPTI
   could be enabled lazily from an interposed `cuInit`/`cuLaunchKernel`, so that
   NVBit always wins the subscriber race by default. Measured: `armed_by=
   constructor` — none of those doors fired, because nvcc links cudart
   statically and it reaches the driver through `cuGetProcAddress`, exactly as
   the IBGDA work measured for the UAR path. The interposers are kept (they work
   for a dynamically linked cudart) but the constructor arms as a last resort,
   and `PCOMM_KERNEL_TIMING_LAZY=1` disables that. **So guard 2 is best-effort
   and the launcher's env var is the only guard that actually holds.** The code
   says so where it could otherwise be misread.

2. **`exclusive` cannot express this conflict.** The provider registry has one
   flag meaning "must own the process", and cupti's conflict is with *one other
   provider*, not with sharing. Marking it exclusive would replay the workload
   just to co-run with ibverbs, which it does happily. It needed an explicit
   pairwise check in `build_passes()` instead.

3. **A two-point clock fit turns anchor error into RATE error.** The globaltimer
   domain first fitted at **+111.7 ppm** on a GPU the clean probe measured at
   −13.9. The clock had not changed; the anchor had been read badly. The first
   `sample_gpu_anchor` did a `cudaMalloc` *inside* the host bracket and took a
   single sample, and under NVBit that bracket is both wide and erratic — so a
   fixed offset error at each end became an apparent slope across the whole
   capture. Hoisting the allocation and keeping the tightest of 8 brackets (the
   noise is one-sided: a bracket can only be too wide) took the bracket to
   **7.2-7.7 us** and the fit to **−7.6 ppm**, in line with the probe. Worth
   remembering for any future clock domain: the fit is more sensitive to anchor
   quality than to anchor count.

4. **Kernel spans cannot be emitted when they complete.** A launch's window is
   only known once its last block retires, and nothing in the NVBit channel
   marks that — there is no "kernel done" event to trigger on. The reduction is
   therefore closed at teardown, holding ~64 bytes per launch. Blocks also make
   the reduction necessary rather than optional: block 0 is not ordered first or
   last, so min(entry)/max(exit) across blocks is the only correct window.

5. **The closing anchor has to be taken before the context dies.** Sampling it
   in `emit_kernel_spans` — which runs after the last context is destroyed —
   would have launched a kernel into nothing. It moved into `nvbit_at_ctx_term`,
   beside the existing flush kernel, which is the last moment the context is
   alive.

6. **Interned handles are per-domain and the one-argument `in.str()` lies.**
   Kernel names came back empty from a capture that had interned them
   correctly: `InternDictionary::str(Handle)` defaults to
   `InternDomain::String`, handles restart at 1 in every domain, so a valid
   handle 1 resolved to `""` with no error. The dictionary's own header warns
   about this in prose. Any new record with an interned field must name its
   domain explicitly.

## Risks

- **The engine gaining a CUDA dependency is a layering change**, the same shape
  as it gaining an NVSHMEM one in `PLAN-20260810-0505`. Mitigation: `dlopen`
  libcupti at runtime and no-op when absent, so an ibverbs-only build on a
  CPU-only node is unaffected. This must be a deliberate decision, not drift.
- **CUPTI version skew.** The record struct is versioned (`CUpti_ActivityKernel9`
  on both 12.6 and 13.0, but 4–9 all exist in the header). Read `->kind` and
  cast to the version compiled against; do not assume across CUDA releases.
- **Arm E's failure mode is silent.** Whatever guard is implemented, the
  acceptance test is a gpumem run that still produces `.pcs` files with the
  engine's kernel timing compiled in — not a code review.
- **`cuptiActivityFlushAll` at teardown.** The engine flushes from atexit paths
  where glibc has already torn down main-thread TLS; `pcomm_engine_drain_thread`
  exists for exactly that hazard and the CUPTI drain has to respect it.

## Explicitly out of scope

- Kernel *source* attribution (PC → line inside a kernel). That is the gpumem /
  `source_line` path and is unrelated.
- Memcpy/memset activity records. CUPTI offers them from the same subscriber
  almost free, and they would enrich the timeline — but they are a second
  record type and this plan is already one architectural decision long.
- CUPTI on AMD/HIP. Vendor-neutral vocabulary says the record is
  `GpuKernel`, not `CudaKernel`; the second backend is future work.
