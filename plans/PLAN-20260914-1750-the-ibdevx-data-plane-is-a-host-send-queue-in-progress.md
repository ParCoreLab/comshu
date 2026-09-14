# PLAN — the ibdevx data plane is a host send queue, so watch the queue

- **Created:** 2026-09-14 17:50 +03
- **Status:** in progress. Step 1 is done: the prototype (`PCOMM_SQ_WATCH=1`,
  `providers/mlx5dv/sq_watch.hpp`) is lossless on du04 up to `1024 200`
  (204,800 WQEs per rank) and keeps 99.9% at `1024 2000`. Steps 2–5 make it a
  supported capture mode.
- **Related:** the `OPEN (2026-09-14)` entry at the top of
  `things-to-circle-back-to.md` (the finding and the measurements live there);
  `tests/hpc/cases/11_ibdevx_peermem/parity/README.md` (where the gap was found);
  the `FIXED (2026-09-08)` entry below it (why BF UARs are not armed).

## The decision

**Observe ibdevx's data plane passively, from the doorbell record and the send
queue, and never touch its doorbell.** Every mlx5 post is three stores — WQE
into the send queue, producer index into the doorbell record, eight bytes to the
UAR — and under ibdevx the first two are plain host memory the NIC reads by DMA.
A thread that watches the doorbell record and reads the WQEBBs it passes over
sees every WQE the NIC is told about, from outside the path that tells it.

That property is the whole argument. Nothing is swapped or forwarded, so no
stack can be broken by being watched; no UAR has to be attributed to its owner;
no platform guard is involved. The price is that observation can lose data the
active path cannot — and the prototype shows the loss is counted exactly, so
the price is visible rather than hidden.

## Rejected

- **Arm every BF UAR with the host doorbell's repeater.** BF is what UCX rings;
  this is precisely the bug that hung two-node MPI (`FIXED (2026-09-08)`). Not
  revisitable without a discriminator — see the last bullet.
- **Interpose NVSHMEM's post function.** `nvshmemt_ibdevx_post_send` is
  `static inline`; there is no symbol.
- **Arm at CREATE_QP, when the queue pair's `uar_page` is known.** It would
  work for ibdevx *only* because ibdevx copies `uar->reg_addr` into
  `ep->bf_reg` after creating the queue pair (ibdevx.cpp v3.4.5, lines 464 vs
  486–487), whereas IBGDA copies it at allocation and turned case 12 red when
  this was tried (job 45587885). A design that rests on one transport's
  statement order is not a design.
- **Sleep between polls.** Measured: a 20 µs sleep — about 60 µs after the
  kernel rounds it — decoded 9 of ~4,100 WQEs on case 11. The 1,024-slot ring
  laps inside one sleep.
- **SigmaTrace's approach.** It only watches UARs a GPU maps; it has no
  CPU-submitted path at all. Nothing to port.
- **Held in reserve, not rejected: an exact, active mode.** If passive cannot be
  made lossless at realistic rates, arm *only ibdevx's* BF UARs — attributed by
  the caller of `mlx5dv_devx_alloc_uar` (`dladdr` of the return address →
  `nvshmem_transport_ibdevx.so`), which the FIXED entry already named as the
  fallback discriminator. The existing repeater is already correct for it:
  ibdevx writes exactly eight bytes at offset 0 and never changes
  `bf_reg_offset` (v3.4.5, 3.7.2, git). The platform guard would be scoped to
  NC UARs, since ibdevx never maps its UAR into the GPU. It sits in the data
  path, so it stays opt-in even if built.

## Constraints

- **Never decode a slot that has not proven itself.** ibdevx never initialises
  its doorbell record; until the first post the counter is stale heap. The
  control segment carries both the queue pair number and its own WQE index, and
  `slot_holds()` checks both before anything is decoded. Do not replace that
  check with "trust the counter".
- **Loss is a counter, never an absence.** `lapped` must keep satisfying
  decoded + lapped = posted — it does today (20,543 / 20,540 on the 10× run).
  A change that makes the numbers look better by dropping that identity is a
  regression.
- **Stop watching at umem dereg, not at exit.** The dereg hook runs before the
  transport frees the buffer; the C++ runtime's exit ordering does not
  guarantee anything. `State` is leaked for the same reason — a destroyed
  joinable `std::thread` aborted every watched rank (signal 6) in the first
  prototype.
- **IBGDA's queues are not this path's.** They are GPU-resident; the watcher
  takes only `!gpu_driven` queue pairs whose send queue and doorbell record are
  both known `by_addr` host umems, and refuses everything else with a reason.
- **The watcher thread never records.** Its first full arena stalled it for
  2.1–2.4 ms inside `pcomm_engine_arena_flush` and the proxy lapped the ring
  many times over (step 1, below). Anything that can block — recording,
  interning, allocation — belongs on the decoder.
- **The watcher never shares the rank's core.** Open MPI binds a rank to one
  core by default, and a watcher inheriting that binding competes with the very
  proxy it watches: 7,700–16,000 involuntary context switches and ~20% kept on
  `1024 20`. `place_thread` moves both threads to the rank's NUMA node by
  default; keep `PCOMM_SQ_WATCH_CPU=bound` as the measurement it is, not as an
  option anyone should run with.

## Steps

1. **Throughput — DONE 2026-09-14.** Two causes, found by instrumenting rather
   than by building the planned fix first, and the planned fix alone would not
   have been enough:

   - *A stall, not a slow watcher.* Every lossy run lost its WQEs in ONE event
     (`max_delta` = `lapped`), even with the watcher alone on a CPU. The longest
     batch was 2.1–2.4 ms at batch ~800 — the first 4,096-row arena filling.
     Fixed by the split: the watcher copies into a 16 MiB staging ring and a
     decoder thread records. The same stall now shows up as `max_decode_us`
     ≈ 2–4 ms, where it costs nothing.
   - *No CPU.* With the split in, runs under Open MPI's default binding still
     lost 20–55%; unbound, the same runs were lossless. Fixed by placement
     (see Constraints).

   Measured after both, du04, default binding, `auto` placement:

   | workload | WQEs decoded per rank | lapped | dropped |
   |---|---|---|---|
   | `1024 2`, ×2 | 2,048 × 4 B — exact | 0 | 0 |
   | `1024 20`, ×2 | 20,480 × 4 B = 81,920 B — exact | 0 | 0 |
   | `1024 200` | 204,800 × 4 B = 819,200 B — exact | 0 | 0 |
   | `1024 2000` | 2,045,577 / 2,046,964 of 2,048,000 | 2,427 / 1,041 | 0 |

   **Where it breaks:** ~2M WQEs per rank, at 0.05–0.12%, always counted. The
   largest single jump there was 1,028–1,220 WQEBBs — just over the 1,024-slot
   ring, i.e. an occasional ~190 µs gap between two looks at the same queue.
   Salvaging instead of discarding (drop only the overwritten prefix) was
   planned here and is NOT worth it yet: `lapped_late` is 13–28 of those, the
   rest are `lapped_early`, which no salvage recovers. The ring is the
   application's (`NVSHMEM_IBDEVX_QP_DEPTH`-class settings), so the loss rate
   at a given traffic rate is partly the application's to set.
2. **Cost.** Spinning costs a core per rank. First number, and not yet the
   answer: `1024 200` took 3.89 s watched, 3.85 s under pcomm with the watcher
   off, 3.87 s bare — within noise, because this workload's wall time is
   startup. It needs a workload whose runtime is traffic, and a measure of what
   the extra spinning core costs a job that needed it. Then try
   `umwait`/`monitorx` on the doorbell record (the memory repeater already has
   both) or adaptive backoff, and keep whichever holds step 1's numbers.
3. **Record semantics.** `Mlx5DeviceSend` is named for the GPU path. Add who
   submitted it (GPU doorbell hijack / host doorbell / send-queue watch) — a
   schema change, which pre-alpha takes over a misleading name — and document
   that a watched row's time is observation time.
4. **Surface.** A launcher flag instead of the env var, and the counters into a
   record rather than stderr, so an assertion can read them.
5. **Case 11 asserts it.** 2 × 1024 `rdma_write` of 4 B per rank (8,192 B —
   the workload's own arithmetic), full decode, `lapped = 0`. The case that
   found the gap becomes the one that proves it closed.
6. **Later, and bigger than ibdevx.** The doorbell record format is mlx5's, not
   NVSHMEM's. Hooking `mlx5dv_init_obj` would hand the watcher every
   verbs-created queue pair whose owner posts by hand — UCX's `rc_mlx5` above
   all — and the CQ doorbell record would give completions the same way. That
   is a wire-level view that does not care which library posted.

## Reproducing the prototype's numbers

du04, from a checkout with the prototype: `tests/hpc/run_on_a100.sh --cases 11`
to build (and to confirm the watcher-off path still passes), then case 11's
environment plus `PCOMM_SQ_WATCH=1` (forwarded with `mpirun -x`) and
`pcomm run -p ib,mlx5dv -- ~/nvshmem_putget_sm80 1024 <iters>`. The per-rank
`[pcomm] sq-watch` line on stderr carries `wqebbs`, `sends`, `held` and
`lapped`; the rows are `mlx5_device_send`.
