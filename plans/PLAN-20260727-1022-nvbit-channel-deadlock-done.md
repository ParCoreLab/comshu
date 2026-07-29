# PLAN — the NVBit "channel deadlock" (RESOLVED — it was never the channel)

- **Created:** 2026-07-27 10:22 +03
- **Closed:** 2026-07-29
- **Status:** done

## Outcome

Both cases are promoted; the suite is green with no xfails.

**Case 07** — instrumenting LOCAL-memory accesses (per-thread stack / spilled
registers) inside NVSHMEM's inlined proxy-enqueue path wedged the kernel.
Bisected to one `LDL.64` at `+0x19680` in `nvshmemi_transfer_rma_p<int>` using
`PCOMM_INSTR_LO/HI`: instrumenting `[0,1006)` completes in 22s, `[0,1007)` hangs.
Fixed in `3c5d55f` by instrumenting only GLOBAL/GENERIC/GLOBAL_TO_SHARED — which
is the correct rule anyway, since stack and shared memory can never be
communication. `NVSHMEM_DISABLE_P2P=1` reproduces the old hang on a single node,
which is what finally isolated it: node count was never the variable, the IBRC
proxy transport was. The *mechanism* is still unexplained and is recorded in
things-to-circle-back-to.md for an upstream report.

**Case 05** — never a deadlock and not a workload problem either. A fixed ~435s
of NVBit module processing (NCCL ships 223 device-function specializations /
434k instructions in one module). Measured at N=100 through 262144: 433-443s at
every size while the capture scales 0.6 MB -> 47 MB; uninstrumented is 6s.
Filtering to the single launched kernel gives 438s vs 436s. Deadline raised to
600s and the case documents that shrinking the message does not work.

## What this file got wrong, and it is worth reading

Eight explanations were recorded and refuted, several of which sounded
authoritative at the time:

1. "NVBit's bounded channel starves the producer" — channel measured empty.
2. "The hang precedes any data being produced" — inference from (1), also wrong.
3. "Cases 05 and 07 are one bug" — built on (2), collapsed with it.
4. The channel ring-wrap reset race — `atomicExch` fix did not help.
5. A hard 64 MiB channel ceiling — an artifact of a timeout.
6. Workload volume — single node completes at 4x the volume.
7. PCIe zero-copy contention — a device-memory ring behaves identically.
8. Grid/occupancy — the same work in a 4x smaller grid still hangs.
9. The NVSHMEM barrier kernel — removing the barriers changes nothing.

Two self-inflicted failures also produced convincing data: a debug counter read
at recv-thread exit took `g_mutex` while `nvbit_at_ctx_term` held it across
`pthread_join` (wedged teardown at every size, invalidated a whole job), and an
earlier "fix" deleted `flush()`'s tail reset while claiming to harden it.

The pattern in every case: a plausible mechanism was adopted and then defended,
instead of being tested against the cheapest measurement that could kill it. The
bisect worked because it manipulated the causal variable directly rather than
reasoning about correlates.

## What this file got wrong, so it is not re-derived

Three explanations were recorded and each was refuted by measurement rather than
argument. Worth keeping as a caution about how confident the wrong ones sounded.

1. **"NVBit's bounded channel starves the producer."** Carried since 2026-07-21
   across two meta.json files, two plans and things-to-circle-back-to.md; cited
   Snoopie's real documented limitation. Never measured. Instrumented the recv
   thread: `bytes=0`, doorbell never set, ring never filled, recv threads
   healthy at ~19M polls/sec. No producer was ever blocked on it.

2. **"So the hang precedes any data being produced."** My own inference from
   `bytes=0`, and also wrong. `flush()` only fires when the 1 GiB-per-context
   ring *fills*; a kernel producing far less simply never flushes. Records were
   accumulating in the buffer the whole time. `bytes=0` was uninformative.

3. **"Cases 05 and 07 are one bug."** Built on (2), and collapsed with it. 05
   completes at 1800s; 07 does not. Different failures.

The one thing that held throughout: the producer never blocked on a full
channel. That was enough to kill (1), and not enough to support (2) or (3).

## What is actually true

| | case 05 (NCCL, 1 node) | case 07 (NVSHMEM, 2 nodes) |
|---|---|---|
| 100s deadline | hangs | hangs |
| 1800s deadline | **completes, valid capture** | still times out |
| channel during hang | empty | empty |
| gpumem records | produced | **none** |
| ib records | n/a | 110.pcs present |

Isolation already established for 07: 2 nodes with gpumem alone also hangs, so
coexistence is not involved; 1 node with both providers completes; NVBit's own
`NO_EAGER_LOAD=0` hint does not help; the `g.heap_regions` race was fixed and
did not help. Instrumentation *is* required — NVBit loaded with
`PCOMM_KERNEL_NAME` matching nothing runs fine, instrumenting the kernel hangs.

## Next step (case 07 only)

Host backtrace of a stuck rank. Login nodes block ptrace, so either attach with
gdb on a compute node inside the allocation, or `ulimit -c unlimited` and debug
the core. The sharpest clue to chase: **gpumem produced no records at all here
while ib did**, so look at the gpumem/NVBit side of multinode NVSHMEM
initialisation rather than at the workload.

## The failure

Two independent workloads hang under the gpumem/NVBit provider:

| case | workload | topology | outcome |
|---|---|---|---|
| 05 | NCCL all-reduce | 1 node × 4 H100 | deadlock (rc 124) |
| 07 | NVSHMEM put-ring | 2 nodes × 4 H100 | deadlock (rc 124) |

**MEASURED 2026-07-27 — the long-standing "bounded channel" explanation is
WRONG.** Ran case 05 with `PCOMM_CHANNEL_DEBUG=1` (instrumentation now in
`recv_thread_fun`). Across all four contexts, for the whole hang:

```
recv_calls=933,389,980   empty=933,389,980   bytes=0   max_recv=0
```

Zero bytes ever received; the doorbell was never once non-zero; the recv threads
polled ~19M times/second, so they were scheduled and healthy throughout. Since
`ChannelDev::flush()` sets the doorbell and `recv()` returns >0 exactly when it
is set, **the device never flushed, the ring never filled, and no producer was
ever blocked on it.** The channel is not the deadlock.

The old story — "NCCL's persistent kernel occupies the GPU, NVBit's bounded
channel can't drain, producer/consumer deadlock" — was plausible, matched
Snoopie's known limits, and was repeated in two meta.json files, this plan and
things-to-circle-back-to.md. It was never measured. It is now refuted.

**Case 07 measured too (job 43877771), after the `heap_regions` race was fixed:
identical signature — `bytes=0` across all 8 contexts for 290s, and it still
hangs.** Two conclusions. The race was NOT case 07's cause. And 05 and 07 really
are one bug, not two — but the shared property is "hangs before producing a
single byte", not "starves the channel". Look for one cause, not two.

**Where it actually hangs: before any instrumented access is recorded at all.**
`bytes=0` means not one memory access was ever pushed, so the hang precedes the
instrumented kernel producing data. That matches the original observation that
NCCL "inits, prints version, then hangs". Look at NVBit's interaction with the
comm library's *initialization* path, not at data flow: instrumentation
installation (`handle_launch` / `instrument_function_if_needed`), the CUDA
event callbacks under `g_mutex`, or NVBit's lazy module loading during NCCL
bootstrap.

Single-provider vs. co-run is **not** a factor — this was isolated 2026-07-27:

| topology | providers | result |
|---|---|---|
| 1 node, 4 PEs  | ib + p2p                | exit 0   |
| 2 nodes, 8 PEs | p2p **alone**           | exit 124 |
| 2 nodes, 8 PEs | p2p, `NO_EAGER_LOAD=0`  | exit 124 |

NVBit's own banner hint for deadlocks (`NO_EAGER_LOAD=0`) does not help.
Snoopie hits the same wall, so this is not a defect we introduced — it is the
known limit of the technique, and beating it is a genuine contribution.

## Code reading, 2026-07-27 (mechanism NOT yet confirmed — instrument next)

Read `gpu-backends/nvbit/nvbit-1.7.7.1/utils/channel.hpp` and `nvbit_backend.cu`.
Two findings that change the fix list below, and one unrelated bug.

**1. The channel buffer is ALREADY host-pinned.** `ChannelDev::init` uses
`cudaHostAlloc(..., cudaHostAllocDefault)` in the default build
(`cudaMalloc` only under `USE_ASYNC_STREAM`, which we do not define). Only the
small `ChannelDev` struct is `cudaMallocManaged`. So "move the channel to
host-pinned memory" is largely a no-op — fix idea 1 below needs rewording: the
lever is *removing the device-side wait*, not relocating the buffer.

**2. The actual blocking point is a device spin on a host-mapped doorbell.**
`ChannelDev::push()` → when full, one warp runs `flush()`, which sets
`*doorbell = nbytes` and then spins `while (*doorbell != 0)` waiting for the
host recv thread to clear it. So an instrumented kernel *stalls on the GPU until
a host thread services it*. Anything that prevents `recv_thread_fun` from
running, or that keeps the app thread inside a CUDA call while the kernel is
stalled, is a candidate deadlock. `recv()` itself is a plain `memcpy` from
pinned memory with no CUDA calls, and the recv thread takes `g_mutex` only once
at startup — so there is **no obvious lock cycle**, and the mechanism is still
unproven. Do not guess further; instrument.

**3. Unrelated bug found while reading — data race on `g.heap_regions`.**
`find_nvshmem_heap_owner()` (called from the recv thread, per access) lazily
*mutates* shared state — `std::sort` on `g.heap_regions`, assigns
`region_index`, sets `heap_regions_sorted` — with **no lock**, while
`handle_alloc()` does `g.heap_regions.push_back(...)` under `g_mutex` from the
CUDA-callback thread. A `push_back` that reallocates while the recv thread is
sorting/iterating is a use-after-free. This is not obviously the deadlock (a
race, not a cycle) but it is a real defect and it is in the multinode NVSHMEM
path, which is where case 07 hangs. Worth fixing regardless, and worth ruling
out before hunting further.

## Candidate fixes — SUPERSEDED, all three targeted the channel

Kept only so they are not re-proposed. All three assumed the producer was
blocked on a full channel; the measurement above shows it never was. Any fix
below would have changed nothing.

<details>
<summary>Original list (do not implement)</summary>

1. **Remove the device-side wait.** (Reworded after the code read above — the
   buffer is already host-pinned, so relocating it achieves nothing.) The
   problem is `flush()` spinning on the doorbell inside the kernel. Options:
   a much larger ring so a bounded workload never fills it (CHANNEL_SIZE is
   already 1 GiB *per context*, so measure occupancy first — if we are not
   actually filling it, the deadlock is elsewhere entirely and this whole fix
   list is wrong); or a drop-with-count policy on overflow instead of a stall,
   which trades completeness for liveness and matches ARCHITECTURE §3's stated
   "backpressure = drop-with-count, never silent wrap".
2. **Cooperative-launch flush.** A flush that can preempt the persistent kernel.
   Needs cooperative launch support and probably conflicts with how NCCL
   launches its own kernels — investigate feasibility before investing.
3. **IBGDA transport (07 only).** GPU posts WQEs directly, no host proxy to wait
   on. But it also bypasses host verbs entirely, so the `ib` provider would see
   nothing — this fixes the hang at the cost of the very cross-transport capture
   case 07 exists for. Only useful as a diagnostic to confirm the proxy-wait
   theory, not as the fix.

</details>

## CHANNEL EXONERATED (2026-07-27, second measurement) — read this first

The section below ("the ring-wrap reset is racy") reached the wrong conclusion.
Device-side counters (`PCOMM_CHANNEL_DEBUG`, slots documented in channel.hpp)
settled it:

```
1 GiB ring, N=196608:  push=289,426  full=0  flusher=0  flushbody=0  -> HUNG
case 07 config      :  push=18,432   full=0  flusher=0  flushbody=0  -> HUNG
```

With a large ring the buffer **never fills**, so the entire full/flush/reset
path never executes — and it still hangs. The channel cannot be the mechanism.

The earlier evidence (`flusher=4`, `waiter=22,294`, frozen) was a *downstream
symptom*: once the kernel wedges for the real reason, warps that happen to be
mid-`push` park wherever they are. With a small ring some park in the waiter
spin, which looked like a channel deadlock. Its internal consistency
(flusher+waiter == full, ring==cleared==done) was real but proved only that the
channel's own bookkeeping was sound.

An `atomicExch` fix for the plain-store/atomicAdd race was written and tested:
**all four hanging configurations still hang.** The race is genuine (a plain
store colliding with an atomic RMW is a lost update) so the change is kept as
correctness hardening, but it fixes nothing here.

**Where it actually is:** the kernel wedges with the channel uninvolved, and
instrumentation is required to trigger it (NVBit loaded with `PCOMM_KERNEL_NAME`
matching nothing runs fine). `push` counts freeze rather than climbing, so the
kernel is NOT spinning in an instrumented loop generating records — it is parked
in a wait with no instrumented memory traffic. Combined with case 06 (single
node, same code) passing, the only structural difference is the **IBRC proxy
wait on the multinode path**. That is where to look next: what the instrumented
kernel is waiting on, and why the host-side proxy never satisfies it.

Note `push=18,432` for the real case 07 config — it wedges very early, after
very little data. Volume was never the trigger either; the earlier N-thresholds
were measuring how long different configs took to reach the wedge, not a limit.

## SUPERSEDED: the ring-wrap reset theory

Case 07's hang is in NVBit's `ChannelDev` buffer wrap. Evidence, all measured
with `PCOMM_CHANNEL_SIZE` (new knob) forcing a small 16 MiB ring so wraps happen
often and progress is visible:

```
N=163840  ->  OK    69,304,608 B   (4.13 wraps... completes)
N=196608  ->  HUNG  67,108,608 B   = 4.00 x 16 MiB ring
N=262144  ->  HUNG 100,662,912 B   = 6.00 x 16 MiB ring
```

Healthy runs capture exactly **423.00 bytes/element**; the hung ones stop
mid-stream at an **integer ring-wrap boundary** (within one record of it). It is
a race, not a capacity limit — which is why it needs several wraps to trigger
and why it looked volume-dependent.

The suspect code, `channel.hpp` `flush()`:

```cpp
buff_write_tail_ptr = buff;   // reset
__threadfence();
buff_write_head_ptr = buff;
```

while other warps are concurrently in `push()` doing
`atomicAdd((ULL*)&buff_write_head_ptr, nbytes)`. A warp that reserved space just
before the reset holds a `curr_ptr` past `buff_end`; after the reset the
flushing warp's `while (buff_write_tail_ptr != curr_ptr)` wait can never be
satisfied. Device spins forever -> 100% SM, 0% memory traffic, host sees no
further data. Matches every observation.

**What this rules out** (each killed by measurement, not argument): bounded
channel starving the producer; doorbell not volatile/not visible (it is volatile
both sides with `__threadfence_system`, and flushes demonstrably work — 27.7 MB
passed through a 16 MiB ring); `push()` spinning before ever flushing; ring
capacity (hangs at ~7% occupancy on the 1 GiB ring); launch count (10 launches
fine); host-side deadlock (main thread `R`, spinning in
`cudaDeviceSynchronize`); NVBit's mere presence; provider coexistence; the
`g.heap_regions` race.

**One thing still unexplained:** at the default 1 GiB ring, N=196608 hangs having
delivered *zero* bytes, though 83 MB should fit without a single wrap (and 69 MB
at the same ring completed fine). Either the 1 GiB pinned allocation is not
really giving a 1 GiB ring, or there is a second failure mode. Worth resolving
before trusting any fix.

## Next step

1. Add device-side counters (wrap count, warps parked in each spin) to confirm
   the wrap race directly rather than by inference.
2. Then fix in our vendored copy: make the wrap reset safe against concurrent
   `atomicAdd` reservations — e.g. monotonically increasing head/tail with
   modulo indexing instead of pointer reset, so a stale reservation is still
   ordered correctly.
3. Resolve the 1 GiB anomaly above.

## Superseded next step (kept: it produced the measurements)

Find where the hang actually is, given it is *before* any access is pushed.
Cheapest first: run case 05 under `PCOMM_CHANNEL_DEBUG=1` again and get a host
backtrace of the stuck ranks (`gdb -p` on a compute node, or a core via
`ulimit -c unlimited` — login nodes block ptrace, compute nodes may not). That
says directly whether the app thread is parked in a CUDA call, inside an NVBit
callback, or on `g_mutex`.

Also worth trying, both one-line experiments:
- `PCOMM_KERNEL_NAME` set to a name that matches nothing, so NVBit instruments
  no kernel at all. If it still hangs, the cause is NVBit's presence during
  init, not our instrumentation.
- ~~The `heap_regions` race could plausibly have caused case 07~~ — tested,
  it did not: 07 still hangs after the fix, with the same `bytes=0` signature as
  05. Both cases share one cause and it is upstream of any data flow.

## Old suggested first step (done — it is what produced the measurement)

Confirm the mechanism before building anything. The code read above narrowed
*where* to look but did **not** identify the cause, and one plausible-sounding
fix already turned out to be based on a false premise.

Instrument the channel: log doorbell occupancy, how often `push()` finds the
buffer full, and whether the device is spinning in `flush()` when the hang
occurs. Run case 05 (1 node, fastest repro). The single most informative number
is whether the 1 GiB ring is actually filling — if it is not, the producer is
not blocked on fullness and the deadlock is somewhere else (candidates: the
`heap_regions` race above, or the recv thread never being scheduled).

Note case 07's repro is ~5 min on 2 nodes; case 05 is the tighter loop.

## Definition of done

`05_nccl_p2p_xfail` and `07_nvshmem_multinode_xfail` go **XPASS** — which
`run_suite check` already reports as an alert ("it started working, so remove
the marker and add goldens"). Then the xfail markers come off, goldens land via
the usual two-run determinism gate, and `07`'s `cross_transport.sql` finally
gets a golden — giving cross-transport traffic correlation a regression test.
