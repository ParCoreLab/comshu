# PLAN — fix the NVBit bounded-channel deadlock

- **Created:** 2026-07-27 10:22 +03
- **Status:** backlog
- **Unblocks:** `05_nccl_p2p_xfail`, `07_nvshmem_multinode_xfail` — and with 07,
  the unified NVLink+IB *traffic* timeline, which is the project's headline
  result and currently has no regression test at all.
- **Related:** `plans/PLAN-20260727-0126-shared-engine-abi-todo.md` (its
  Sequencing section notes this gap), `things-to-circle-back-to.md`

## The failure

Two independent workloads hang under the gpumem/NVBit provider:

| case | workload | topology | outcome |
|---|---|---|---|
| 05 | NCCL all-reduce | 1 node × 4 H100 | deadlock (rc 124) |
| 07 | NVSHMEM put-ring | 2 nodes × 4 H100 | deadlock (rc 124) |

Both are the same shape: **a comm library that spins on the device, versus
NVBit's bounded host↔device channel that needs the kernel to make progress in
order to drain.** NCCL's persistent collective kernel occupies the GPU
outright. Multinode NVSHMEM's IBRC path has device code waiting on a host proxy
thread. Either way the producer fills the channel, the consumer can't drain it,
and nobody advances.

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

## Candidate fixes (unevaluated — that is the first task)

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

## Suggested first step (unchanged, and now clearly the right one)

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
