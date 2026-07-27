# PLAN — the NVBit "channel deadlock" (mostly RESOLVED — it was not one)

- **Created:** 2026-07-27 10:22 +03
- **Status:** backlog
- **Resolved for case 05:** it was never a deadlock. Measured 2026-07-27
  (job 43878352): raise the deadline 180s -> 1800s and NCCL-over-NVLink under
  NVBit **completes** with a valid capture (700.pcs 7440B x4 ranks). Just NVBit
  slowdown against a 3-minute bound. Case 05 is promotable — see its meta.json.
- **Still open for case 07 only:** multinode NVSHMEM still times out at 1800s
  (job 43879008) and emits no gpumem records at all. A different bug from 05.
- **Related:** `plans/PLAN-20260727-0126-shared-engine-abi-todo.md`,
  `things-to-circle-back-to.md`

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

## Next step (rewritten after the measurement)

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
