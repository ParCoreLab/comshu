# PLAN — the ibdevx data plane is a host send queue, so watch the queue

- **Created:** 2026-09-14 17:50 +03
- **Status:** in progress. Steps 1–4 are done: the watcher
  (`PCOMM_SQ_WATCH=1`, `providers/mlx5dv/sq_watch.hpp`) is lossless on du04 up
  to `1024 200` (204,800 WQEs per rank), keeps ≥99.7% at 20.5M WQEs per rank,
  and — placed on the rank's L3 — slows the traffic phase ~1.15× at full rate.
  and every row says which door saw it and when (a Window, ~3 µs wide). It
  is enabled with `pcomm run --watch-send-queues`, and its coverage is a
  record (`mlx5_send_coverage`). Step 5 makes case 11 prove it every run.
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
2. **Cost — DONE 2026-09-14.** The watcher's cost is distance, and placing it
   on the rank's L3 cut the traffic-phase slowdown from ~2.2× to ~1.15×
   (result at the end of this step).

   *Method, because the first numbers were wrong.* Single runs at `1024 20000`
   said the watcher tripled the traffic phase; rerun, the same configuration
   matched the unwatched one, and bare itself varied 7.1–10.1 s. Three causes:
   `auto` lands on a random L3 and migrates mid-run (`cpu=0->56`), NVSHMEM's
   ibdevx deadlocks ~1 in 6 at this size with no pcomm at all
   (things-to-circle-back-to.md), and one-shot wall times are noise. So:
   `~/pcomm-parity/cost/cost_bench.sh` on du04 — every configuration 5 times,
   interleaved round-robin, each run under a 60 s watchdog that reports HUNG
   instead of a time, medians.

   | du04, `1024 20000`, 20.5M WQEs/rank | median wall | traffic phase† | watcher CPU/rank | lost |
   |---|---|---|---|---|
   | bare | 7.17 s | ~3.4 s | — | — |
   | pcomm, watcher off | 7.17 s | ~3.4 s | — | — |
   | watcher pinned on the rank's L3 (CPU 2/3)‡ | 10.23 s | ~6.4 s (1.9×) | 7.4 s | lap ≤0.2%, **drop 0.5–2.9%** |
   | watcher `auto` (NUMA node) | 11.26 s | ~7.4 s (2.2×) | 8.4 s | lap ≤0.2% |
   | watcher on another L3, same socket | 14.33 s | ~10.5 s (3.1×) | 11.4 s | lap ≤0.1% |
   | watcher on the other socket | 19.96 s | ~16.1 s (4.8×) | 17.0 s | lap ≤0.5% |

   † wall minus ~3.8 s of startup (the `1024 200` wall).
   ‡ with its decoder left on the rank's own core — see the correction below.

   *What it says.* pcomm without the watcher costs nothing measurable. With it,
   the cost is not CPU time but **distance**: the slowdown grows monotonically
   with how far the watcher sits from the proxy it reads — same L3, another L3,
   another socket — which is what cache-line transfers predict: every WQEBB and
   doorbell-record line the watcher reads is one the proxy must take back before
   it can write it again. Spinning costs a whole core per rank (watcher CPU =
   its wall). Sleeping was already ruled out: 9–99% lost at 20–200 µs even with
   precise timers.

   *What it changes.* `auto` should prefer the CPUs sharing the rank's L3,
   minus the rank's own binding, and fall back to the NUMA node when there are
   none (`PCOMM_SQ_WATCH_CPU=node` keeps today's behaviour). On a
   socket-wide-L3 machine (MN5's Sapphire Rapids) that is the socket, so nothing
   changes there. Its known limit: on a node whose ranks fill an L3 domain the
   watcher lands beside them again, which the `bound` numbers already priced.

   *~~And what it uncovers~~ — RETRACTED, same day.* This said that near the
   proxy the run became fast enough for recording to be the bottleneck (~1.15
   GB per rank in ~6 s overflowing the staging ring), and filed it under step 3.
   Wrong. The pinned configuration left the **decoder** on the rank's own core —
   `place_thread`'s `<base>` mode did that on purpose — where it competed with
   the proxy and fell behind. The result below records the same volume faster,
   with the decoder on the node, and drops nothing. `<base>` now places the
   decoder on the node too.

   *Result of the change, same method* (du04, `1024 20000`, 5 interleaved
   runs, deadlocked runs excluded — 1 bare, 1 pinned):

   | configuration | median wall | traffic phase | lost |
   |---|---|---|---|
   | bare | 7.17 s | ~3.4 s | — |
   | **`auto` = the rank's L3, minus its binding** | **7.68 s** | **~3.9 s (1.15×)** | lap ≤0.3%, 0 dropped |
   | `node` (the old `auto`) | 11.26 s | ~7.5 s (2.2×) | lap ≤0.2%, 0 dropped |
   | pinned CPU 2/3, decoder on the rank's core | 10.49 s | ~6.7 s (2×) | lap ≤0.3%, drop 0.8–2.5% |

   Every `auto` run started and ended inside the rank's L3 `{0-3,128-131}`
   (`131->3`, `0->130`, `130->2`, …). The watcher still spins a core per rank
   (~4.9 s CPU on a ~5 s window); `umwait`/`monitorx` would save that core's
   power, not the transfers, and is not worth its complexity until a job is
   shown to want the core back.
3. **Record semantics — DONE 2026-09-15.** `Mlx5DeviceSend.submitter`
   (`SendSubmitter`: 0 unknown, 1 `gpu_doorbell`, 2 `host_doorbell`,
   3 `sq_watch`) says which door saw each row; 0 is never written on purpose,
   so a call site that forgets to say is visible. The reader exports
   `submitter`, `submitter_name` and `ticks_hi`, appended so no column moves.

   *Time.* It was "observation time", and after step 1's split that meant the
   decoder's recording time — up to milliseconds after the watcher saw the
   WQE. Now the doorbell doors record an Instant at the doorbell, and the
   watcher a **Window**: TSC reads either side of each doorbell-record read,
   carried through staging, bound the publication between its previous look
   and this one. `TimeKind::Window` already existed for exactly this ("known
   only within [a, b]"). Measured, du04, `1024 20`: 41,023 rows, all
   `sq_watch`, none inverted; width median 3.1 µs, p90 3.8, p99 16.5, max
   45.6 µs (TSC 2.25 GHz). Case 12's 1,343 rows: all `gpu_doorbell`, all
   Instants.

   *The name stays.* `mlx5_device_send` says "device" and ibdevx is a CPU
   proxy, but renaming it reaches ~60 files — UI panels, the agent catalog,
   figures, a dozen assertions and probes — for no gain in meaning once every
   row names its door. The record's comment says why the name is older than
   its meaning. Schema hash `0xC49A5128D6293AF1`. MN5 board job 45884393 at
   `e246b6b`: 23 PASS / 2 SKIP (09, 10 — no `PeerMappingOverride` there).
   That board's case 12 runs the host doorbell (MN5 cannot map a UAR into the
   GPU): 63 rows, all `host_doorbell`, all Instants — so each of the three
   doors has now been seen naming itself on real hardware, not just the two
   du04 can exercise.
4. **Surface — DONE 2026-09-15.** `pcomm run --watch-send-queues` sets
   `PCOMM_SQ_WATCH=1` (not exclusive with the doorbell flags: the watcher takes
   only host-resident queues and refuses the GPU-resident ones those watch).
   The counters are a record, `Mlx5SendCoverage` (218, Artifact,
   `mlx5_send_coverage`), one row per rank, built so an assertion can ask the
   whole question: `published == decoded + lapped + dropped`, plus rows,
   unknown-opcode and truncated rows, queues watched, refused and held.

   *When it is written* was the design question, not what. The provider's
   destructor, where the counters used to be printed, runs AFTER the tracer's
   atexit `finalize()` — a record from there would never reach the capture.
   The watcher registers its own shutdown with `atexit` when it first starts;
   atexit is LIFO and the tracer registered earlier, so the watcher's handler
   runs first, after the application's last send and while the capture is
   open. Like `capture_status`, its absence then means something: a rank with
   watched queues and no row did not exit normally.

   The row counts are the watcher's own. They used to come from the doorbell
   hijack's process-wide counters, which would mix doors in a run using more
   than one; `parse_wqes` now returns what each call emitted.

   Measured, du04, `1024 20`, watcher enabled by the flag alone (env set to
   0): rank 0 published 20,543 = decoded 20,543 + 0 + 0, rank 1 20,540 =
   20,540 + 0 + 0; `rows` 20,513 / 20,510, equal to the CSV's `sq_watch` rows
   on each rank. Cases 11 and 12 PASS, core ctest 33/33. Schema hash
   `0x05D78F93455BDACA`. MN5 board job 45885534 at `70857db`: 23 PASS /
   2 SKIP (09, 10 — no `PeerMappingOverride` there).
5. **Case 11 asserts it — DONE on du04 2026-09-15; MN5 board pending.** The
   case runs with `--watch-send-queues` and gained four assertions, each
   written so that a watcher that never ran FAILS rather than skips:

   - `suite/asserts/send_coverage_adds_up.sql` — published = decoded + lapped
     + dropped, on every run of every case the watcher ran in; true of a lossy
     run too, so how much may be lost stays each case's own claim.
   - `data_plane_puts_arrived.sql` — every rank carries exactly
     `elems × iters` RDMA writes of `elem_bytes` (2,048 × 4 B), derived from
     `params`; counted by length so NVSHMEM's own 128 B writes are not.
   - `data_plane_covered.sql` — every rank with DEVX queue pairs has exactly
     one coverage row, nothing lapped, dropped or refused, decoded = published
     > 0, and its `rows` equal to the capture's. It requires `mlx5_devx_qp`,
     not the coverage table, so a missing row is a failure and not a [skip].
   - `data_plane_rows_are_windows.sql` — every watched row is a real Window
     (`0 < ticks < ticks_hi`) and fully decoded.

   `expect_tables` gained `mlx5_device_send` and `mlx5_send_coverage`, and
   the case's own notes stopped saying no tool sees this data plane.

   du04, twice, `--no-build` the second time: PASS, 26 assertions (18
   before), both runs. Three suite assertions that had always skipped here now
   run and pass on the watcher's rows (atomic operand width, AV width stable
   per QP, DC sends name a destination). Coverage: rank 0 published 2,075 =
   decoded 2,075, rows 2,063; rank 1 2,072 / 2,060; 5 queues each, nothing
   lapped, dropped, refused or held.

   *Mutation-tested* against the saved capture — each corruption fails exactly
   the assertion it should, and nothing else:

   | mutation | adds_up | covered | puts_arrived | windows |
   |---|---|---|---|---|
   | rank 1's coverage row deleted | ok | **fail** | ok | ok |
   | 5 lapped, still adding up | ok | **fail** | ok | ok |
   | decoded − 1 (a silent loss) | **fail** | **fail** | ok | ok |
   | 10 of rank 0's puts removed | ok | **fail** | **fail** | ok |
   | one window collapsed | ok | ok | ok | **fail** |

   *Still to see:* MN5, where the same case runs NVSHMEM's DC initiators (60
   per rank, service_type 0x5) through the watcher for the first time — a
   different WQE layout, with an address vector, under the same assertions.
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
