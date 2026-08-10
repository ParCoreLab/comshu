# pcomm — things to circle back to

Known limitations / deferred work, recorded so they aren't lost. Newest first.

## OPEN: perftest's queue pair is never created, only modified

`case 08`'s `ib_queue_pair` table is **empty**, on every capture we have
(44111484, 44113361), while `ib_modify_qp` holds the QP's three state
transitions. So the modify records refer to an entity no record ever created.

Cause is the same shape as the two gaps case 08 was built to catch. We hook
`ibv_create_qp` (`ib_hooks.hpp:44`); perftest reaches the WR API through
`mlx5dv_create_qp` and, as the comment above `ibv_qp_to_qp_ex` already records,
never calls `ibv_create_qp_ex` either. The *sends* were fixed by hooking
`ibv_qp_to_qp_ex` — the one door every WR-API path must come through — but the
QP **entity** still has only the one narrow door hooked.

Found 2026-08-03 by `expect_tables`, the cheapest assertion in the suite: three
goldens sat green beside an empty table for as long as the case has existed,
because a query over an empty table returns an empty result and a golden
recorded from the same empty table matches it. Marked `expect_fail_asserts` on
case 08 with the table list left as it should be, not edited down to what we
emit.

## FIXED (2026-08-03, `21f3b74`+`ebaaf3c`): NVSHMEM heap attribution depended on when a record was drained
**Resolution first, history below.** Ownership no longer comes from NVSHMEM's VA
layout at all. `nvshmem_ptr(a, pe)` is asked directly (`dlsym` at runtime, no
build dependency), and `cuPointerGetAttribute` names the GPU the answer lives
on, so `mem_device` is a device rather than a PE wearing one's clothes. The
premise — `nvshmem_ptr(a, my_pe) == a` for local symmetric memory and NULL for
everything else, including peers' own mappings — was measured on MN5 in four
configurations *before* the code was written (jobs 44135828, 44136107), because
the scheme being replaced failed precisely by assuming an unverified layout
property. See `plans/PLAN-20260803-1448-nvshmem-heap-attribution-todo.md`.

Two things the fix turned up that the plan did not predict:

- **`nvshmem_ptr` segfaults outside NVSHMEM's init window** (null+0x20), and
  `nvshmem_my_pe` / `nvshmem_n_pes` keep answering plausibly across the same
  boundary, so they cannot be used as the liveness test. `nvshmemx_init_status`
  can. The recv thread's last refresh naturally lands after the application has
  finalized, so this is the normal path, not an edge case.
- **`cuMemMap` is not NVSHMEM's alone.** NCCL registers peer buffers through it
  too, and case 05's peer traffic existed only because the NVSHMEM positional
  guess was being applied to NCCL's regions — `mem_dev` up to 8 on a 4-GPU node.
  Every mapped region now gets CUDA's answer for which GPU it lives on.
- **Ownership resolves at kernel launch, not on the drain path**, which is where
  the plan put it. The ring never fills at the default 1 GiB, so the whole
  capture arrives in the single flush at teardown, together with the terminate
  token — the one batch needing the map is the one batch that cannot build it.
  Launch time has both properties: NVSHMEM alive, and every region the kernel
  can touch already mapped.

## RESOLVED: case 05's `ops` asserted peer loads that were never real

Answered 2026-08-03 by the user, and scoped rather than taken wholesale. NCCL
moves data over NVLink P2P by **pushing**: the sender stores into the peer's
mapped buffer and stores again to a ready flag. Writes are posted, so the sender
never stalls and the link saturates; a remote read is non-posted and costs a
round trip. Data plane and synchronisation are therefore both stores, and the
consumer polls a flag in memory it owns — a *local* load, filtered out by
`RemoteFilter`.

**Deliberately scoped to the pinning, not asserted of NCCL in general.** The P2P
transport also has a direct-read mode, chosen by topology and configuration, so
the claim rests on `job.sbatch` pinning `NCCL_ALGO=Ring`, `NCCL_PROTO=Simple`,
`MIN/MAX_NCHANNELS=1`, intra-node NVLink. Change the pinning and the check
should go red; the answer is then to re-derive it, not relax it.

Two details the old comments had backwards, both worth keeping in view:

- They blamed **"NCCL's LL protocol"** for unrepeatable counts. The case pins
  **Simple**, not LL.
- They cited loads varying 295,044 / 550,064 / 493,031 "against a perfectly flat
  294,990 stores". The *stable* half was the real half: the loads were local
  reads wearing a phantom device id, and the unrepeatability was the bug rather
  than a property of NCCL. Measured after the fix: 0 loads, 294,987 stores per
  rank, uniform across all four (job 44139911).

`ops.golden` was rewritten by derivation (four ranks, `store` only) rather than
regenerated from the run, and the assertion now requires peer stores present and
peer loads absent.

The original entry:


`05_nccl_p2p` was passing on fabricated attribution — `find_nvshmem_heap_owner`
applied to NCCL's `cuMemMap` regions, which is where `peer_traffic.sql`'s own
note about "(3,0) vs (2,8)" comes from; there is no device 8 on a 4-GPU node.
With attribution fixed its peer traffic is real again (`peer_traffic` passes,
1,179,948 accesses where the run before had 0), and one check still differs:

```
ops:  golden {load, store} per rank;  measured {store} per rank
```

`ops.sql` claims the invariant is that "every rank performs both peer loads and
peer stores". That was recorded from the fabricated owners, and case 03 — NCCL
too, on the alloc-registry path the bug never touched — has asserted peer
**stores only** all along. Deliberately not changed: unlike cases 04 and 06,
whose answer is derivable from a three-line kernel, deciding this would mean
reading it off the run. Needs someone to say whether NCCL's ring allreduce does
direct peer reads at all, and the answer belongs in `ops.sql`'s comment.

The original entry:

`find_nvshmem_heap_owner` (`nvbit_backend.cu`) resolved which PE owns an address
as:

```cpp
return (mype + r.region_index) % npes;   // npes = snap->size()
```

`region_index` is **positional**: `publish_heap_snapshot()` sorts the regions by
base and re-indexes them 0..n-1 *every time a new one appears*. NVSHMEM maps its
symmetric heap progressively, from `cuMemMap` callbacks on the CUDA-callback
thread, while the recv thread is already attributing accesses.

So the owner an access resolves to is a function of **how many regions had been
discovered at the moment it was drained**, not of the address. Measured, same
workload, same 8-PE shape, two runs:

| run | `mem_dev` for `run_dev=0` | implies |
|---|---|---|
| 44113361 | 1 | `(0+1) % 8` — 8 regions |
| 44111484 | 64 | `(0+64) % ~76` — ~76 regions |

Only the second is wrong, and nothing detects it: `run_device != mem_device` is
what `RemoteFilter` keeps, so a garbage owner turns local loads into "remote"
traffic. Case 07 doubled (196608 -> 393216 per rank) and the two node-crossing
ranks went from 0 NVLink accesses to 196608.

**Not caused by the change that revealed it.** The gpumem window work
(`2bfb376`, since reverted) added a clock read and 8 bytes per warp packet —
enough to shift the drain relative to region discovery. Bisect:
07 PASS at `5a28119` and at `ab9e260`, FAIL at `2bfb376`. Any future change to
recv-thread timing will do the same thing.

**Do not fix this by regenerating case 07's golden.** The golden encodes one
arbitrary interleaving; a new one would encode a different arbitrary
interleaving and still be wrong. Ownership has to stop depending on discovery
order — see the unblocking steps in
`plans/PLAN-20260803-1021-gpumem-window-timestamps-todo.md`.

It was worse than a race: the model assumed one heap region per PE, which is
where `npes = snap->size()` came from, and this workload maps ~76 regions for 8
PEs, so the PE count was wrong and everything derived from it was too.

**It had already been worked around, in a passing test.** `case 04`'s
`peer_fanout.sql` said "the ring put has each PE store to one neighbor" while
its golden asserted `peers = 2`, with a comment that mem_device ids were
"unreliable and run-varying". A one-neighbour ring reporting fan-out 2 *was*
this bug, visible in a green check — the second "peer" being the rank's own
local `src` loads wearing someone else's device id. That check is now
`peer_topology.sql` and asserts the exact ring, and cases 04 and 06 lost the
196608 phantom `load` rows per rank that the misattribution had been
manufacturing.

Related, and now diagnosed: case 06's `regions_per_rank` (115 -> 114) and
`ib_entities` (mem_region 460 -> 456) were the same family — goldens pinning an
allocation-dependent number. Proof: at the *same commit* (`5a28119`), job
44073877 on rack `as07r3b` recorded 115 regions per rank and job 44113361 on
`as04r5b` recorded 114, while `queue_pair` (108) and `modify_qp` (132) per rank
were identical on both, and six later runs on `as04r5b` all give 114.

The reason regions in particular drift is in `ib_entities.sql`'s own comment:
it counts `reg_mr` *calls*, not live regions — freed pointers are re-registered
and both are recorded, because dereg emits no close record yet. So the number
tracks NVSHMEM's allocator churn.

Fixed by asserting what the workload actually implies. `regions_per_rank`'s
comment already said it ("per-rank evenness is the structure of the workload")
while its golden pinned an absolute number; that check is now
`asserts/ib_setup_symmetric.sql`, which requires every rank to register, create
and transition the same counts, and `mem_region` is out of the `ib_entities`
golden. Verified stable across both racks and across the attribution fix.

## OPEN: goldens that encode run-specific identifiers

Three cases have now failed on identifiers the run happens to hand out, not on
anything the tool did wrong:

- **01 / 02** — `comm_edges` maps `qp_num -> rank` in one global map. On
  2026-08-03 both ranks were allocated **`qp_num = 449`** on their separate
  HCAs, so the owner became whichever rank `scan_ranks` visited last
  (`fs::directory_iterator`, unordered — the inventory listed rank-1 first).
  Case 01 reported `0->0` instead of `0->1`. The reader's own comment claims
  this is "exact for a single HCA / few ranks... **Deterministic either way ->
  golden-safe**", which is false: it is deterministic only while the two QP
  numbers differ.
- **07** — see the entry above.

Earlier instances are already recorded: case 07's `comm_edges` moved
(`3,4 -> 3,0`) on a different allocation, and case 02's `degree` form was
replaced once for the same reason. This is a recurring class, not three
accidents. The fix direction is the one that already worked for 02: assert
shapes that cannot encode a wire id (per-rank totals, counts, invariants like
"every owner is in `[0, npes)`") rather than specific peer identities.

**Taken, 2026-08-03** (`PLAN-20260803-1520-correctness-checks-not-goldens-done.md`).
Each of these cases now has an `asserts/` claim beside the golden, and every one
of them passes on the captures the goldens fail:

- **01** `topology` asserts fan-out 1 carrying `iters` sends of `msg_bytes`, in
  rank space, and does **not** assert who the peer is. Passes on 44111484 and
  44113361, both of which collide (`449`, `461`) and both of which report
  `0 -> 0`. The self-edge is left unasserted on purpose and the reason is in the
  file: `dst_rank` becomes honest when the local LID is captured, not before.
- **02** `symmetric_participation` asserts that all `ranks` post, and post
  equally. Which is a stronger statement than it sounds: **the `degree` golden's
  flat `peers = 2` no longer reproduces at all.** Job 44113361's `comm_edges`
  name only `dst_rank` 0 and 3, giving degrees alternating 1 and 2, while the
  per-rank send totals are 3360 for all eight. The degree form was already the
  *replacement* for a broken one; at 8 ranks over 2 nodes the `qp_num -> rank`
  map has degraded past the point where any peer-identity claim survives.
- **07** `cross_transport_shape` asserts that exactly `nodes` ranks record zero
  NVLink accesses and the rest record `elems*iters`, and
  `ib_carries_the_crossing` asserts that the zero-NVLink ranks are the ones with
  the bulk IB volume — without naming PE 3 or PE 7, which are an artefact of
  `--map-by ppr:4:node`. On the corrupted 44111484 capture these two plus
  `owner_in_range` and `no_local_loads` fire with four specific diagnoses, where
  the golden produced a sixteen-number diff.

## FIXED: fork-aware capture — the child recorded nothing

Fixed 2026-08-03 (`pcomm_engine_reset_arenas_for_fork`, plan
`PLAN-20260803-1050-fork-child-records-lost-done.md`). Was: parent 130/130,
**child 0 of 50**. The
feature was validated at exactly those numbers when it landed (ec32ab2), so it
regressed since, invisibly — the assertion that would have caught it was
compiled out by `NDEBUG` (see the next entry).

**Cause — not the obvious one.** The tempting diagnosis is that
`Recorder::append` caches the arena in a `static thread_local`, so after `fork()`
the child points at the parent's arena. True, and harmless: `Arena::drain()`
holds no sink pointer, it asks `Tracer::instance().recorder().sink()` at drain
time, and by then `atfork_child` has re-started into the child's directory. A
cached arena pointer drains to the right place.

The break was one line in `ThreadBufferRegistry::reset_for_fork()`:
`m_buffers.clear()`. The child forgot its arenas existed, so `finalize()`'s
`drain_all()` iterated an empty list and the child's records were never written.
The intent — don't flush the parent's buffered records through COW-shared
`FILE*`s — was right, but it discarded the *registration* when what needed
discarding was the *content*.

`atfork_child`'s comment claimed *"The fresh Recorder has a new address, so the
per-thread buffer_store rebuilds against the child's sink"* — true of the
map-keyed `{recorder, type_id}` design, false since the shared-engine C ABI
(4dd6a69) made arenas `type_id`-keyed in a thread-local map. The comment
outlived its mechanism and pointed the diagnosis the wrong way.

**Fix:** `pcomm_engine_reset_arenas_for_fork()` — clear each arena's count and
re-register it, called from `atfork_child` after `start()`. No hot-path change,
no generation counter (the first design, dropped once the dynamic sink lookup
was understood).

## OPEN (minor): Tracer cannot restart after finalize()

`start()` is attach-if-running by design (two preloaded providers must join one
capture), and `finalize()` does not clear `m_recorder`. So a second `start()`
with a *different* `output_dir` silently joins the first capture and prints
"joining the latter" — the second capture's directory is never created.

Harmless in production, where `finalize()` runs once at exit. It bit
`tests/core_smoke.cpp`, which had two capture blocks and whose second block was
therefore asserting against a directory that did not exist; the test now uses
one capture. Worth deciding deliberately whether a restart should be supported
rather than leaving it as an accident of the refcount.

## FIXED: every assert() in the test suite was compiled out

`tools/build_all.sh` builds `RelWithDebInfo`, and the README's `Release` does
the same — both define `NDEBUG`, which makes `assert()` a no-op. **44 runtime
assertions across five files** were dead: `core_smoke` (19), `hook_smoke` (13),
`schema_conformance` (6), `fork_smoke` (4), `thread_flush_smoke` (2). The suite
reported "tests ok (13 tests)" while verifying only that nothing segfaulted.

Found by reintroducing, on purpose, the bug a newly written test existed to
catch — and watching it pass.

Fixed with `-UNDEBUG` on the test targets only (the library and providers keep
`NDEBUG`, so hot-path asserts stay compiled out). Turning them on immediately
surfaced the two entries above.

## OPEN: gpumem stamps *drain* time, as an Instant — plan written

Every `GpuAccess` carries `TimeKind::Instant` at a host TSC read taken when the
NVBit drain thread dequeued it, not when the SM issued the access. The device
captures no clock at all (`dev_common.h` and `inject_funcs.cu` have no time
field), `RawAccess` has no timestamp, and `attr/pipeline.hpp:29` calls the
one-argument `record(rec)`, which is `append(now_instant(), r)`.

So the stamp is wrong three ways: wrong moment (drain, with a 1 GiB ring *and* a
device-side buffer in front of it, so the lag is unbounded); wrong precision
claim (`Instant` asserts exactness nothing knows); and wrong order (a batch is
stamped in flush order, so accesses far apart on the device collapse together).

`Temporal::window` exists for precisely this and has **zero call sites**.

Invisible to the suite because no gpumem golden queries time — 03/04/05/06/07
assert counts, sizes and `run_dev`/`mem_dev` pairs only.

**Fix planned:** `plans/PLAN-20260803-1021-gpumem-window-timestamps-todo.md`.
Short version: the upper bound is free (the current drain timestamp is a valid
*upper* bound), and the lower bound is the kernel launch — already plumbed to
the device (`nvbit_set_at_launch`) and already a parameter of
`pcomm_instrument_mem`, just never stored. Changes no on-disk layout: `Temporal`
is already in every record, only its field *values* change.

## OPEN: nvc++ cannot correctly compile libpcomm_engine.so (rdtsc)

Built with NVHPC's `nvc++` — which is what `CMAKE_CXX_COMPILER` resolves to in
the `build-gpumem` tree, since that configure runs under
`module load nvidia-hpc-sdk` — the engine comes out with an **undefined
reference** to `__builtin_ia32_rdtsc` instead of inlining it:

```
symbol lookup error: libpcomm_engine.so: undefined symbol: __builtin_ia32_rdtsc
```

Every hooked call dies there, so a capture is empty. `nm -D --undefined-only`
on the two engines is the whole diagnosis: gcc-13.2's has 0 such symbols,
nvc++'s has 1.

**Not currently live.** `run_on_mn5.sh` copies only `libgpumem_trace.so` out of
`build-gpumem`, and its `$ORIGIN/../../libpcomm` rpath resolves to the
*gcc-built* engine in `build/libpcomm`, so the suite has always run the right
one. It surfaced only when `cmake --install build-gpumem` dropped the nvc++
engine over the good one (fixed by `PCOMM_INSTALL_CORE`, which now defaults OFF
for a gpumem-only tree — but that is a guard, not a fix).

**The fix** is to write the TSC read so any compiler can build it —
`__rdtsc()` from `<x86intrin.h>`, or inline asm on `rdtsc`/`rdtscp` — in
`pcomm/helpers/timestamp.hpp` / `helpers/clock.hpp`. It is on the hot path of
every hook, so it wants its own MN5 suite run to confirm the timestamps and the
goldens are unchanged. Until then, one toolchain builds the engine and the
build system enforces it.

## OPEN (upstream): why instrumenting a LOCAL-memory access wedges NVSHMEM

**This is the one to file a bug report about.** The case 07 hang is fixed on our
side (below), but the *mechanism* is not understood, and it is very likely not
specific to pcomm.

Bisected to a single instruction (jobs 43918460..43920181, `PCOMM_INSTR_LO/HI`):

| instrumented range | outcome |
|---|---|
| `[0,1006)` | completes, 22s |
| `[0,1007)` | hangs forever |

Index 1006 is `LDL.64` at `+0x19680` in `nvshmemi_transfer_rma_p<int>` — a
**local**-memory access, i.e. a spilled-register reload. Instrumenting every
instruction below it is fine, *including* the 64-bit global atomic at index 1000
(`ATOMG.E.MAX.64.STRONG.GPU`) and the system-scope strong stores at 996 and 1007
(`ST.E.STRONG.SYS`, `STG.E.64.STRONG.SYS`) that sit on either side of it. Those
were the obvious suspects — a proxy-queue reserve/publish handshake — and they
are innocent. One stack reload is the whole trigger.

**What we know:**
- Needs NVSHMEM's IBRC proxy transport. `NVSHMEM_DISABLE_P2P=1` reproduces it on
  a *single node*, 2 GPUs, with identical push counts and threshold to the
  2-node case, so node count was never the variable.
- Needs `push()` to actually run: a provider built with the identical injected
  call but `-DPCOMM_INJECT_NO_PUSH` completes in 10s.
- The channel is not involved: `full=0 flusher=0 flushbody=0` in every hanging
  run at the default 1 GiB ring — the ring never fills and `flush()` never
  executes.
- No host-side deadlock: 26 threads, none blocked on a lock; the main thread is
  legitimately in `cudaDeviceSynchronize` waiting for a kernel that never
  retires.
- The kernel issues *every* memory access it was ever going to (freezes at or
  just past `47 x warps`) plus a few hundred extra from some retry path, then
  will not exit.

**What we do not know:** why a call injected before a stack reload deadlocks a
proxy handshake. Plausible stories — added latency in a timing-sensitive window,
register pressure changing the spill pattern that the instrumented code is
itself reading — are speculation, and speculation lost eight times on this bug.

**Why it matters beyond us:** any NVBit tool that instruments local memory around
NVSHMEM's proxy path should hit this. Snoopie instruments the same way — our
`inject_funcs.cu` is a port of its `instrument_mem` and carries the same
NONE/CONSTANT blocklist — so it is a plausible second victim.

**Do NOT conflate this with Snoopie's documented multinode limitation.** That one
is about *coverage*, not hanging: an address tracer sees load/store instructions,
and inter-node NVSHMEM does not move payload with GPU loads/stores into a PGAS
window — the device enqueues a descriptor and the NIC performs the transfer, so
there is nothing at the instruction level to observe. Our own case 07 capture
demonstrates it directly: the two node-crossing senders (PE3, PE7) record
`nvlink_accesses = 0` while carrying ~196,690 IB sends each. This is precisely
why pcomm pairs `gpumem` with `ibverbs` and why the cross-transport oracle
exists — the IB half is unreachable from instruction tracing by construction.
The hang is a separate, genuine bug that happens to live on the same code path.

**For the report, we already have:** a single-node 2-GPU reproducer, the exact
instruction (opcode/offset/function), a bisect harness (`PCOMM_INSTR_LO/HI`,
`PCOMM_INSTR_DUMP`), and a one-line fix that removes it. Missing: the mechanism,
and confirmation on a second site/NVSHMEM version. du04 (4x A100, live mlx5_0,
`ptrace_scope=0`, sudo, cuda-gdb + compute-sanitizer available under
`/home/iturimbetov18/cuda-12.3/`) is where a debugger can actually be attached —
MN5 blocks ptrace everywhere. Caveat: du04 is NVSHMEM **2.10** vs MN5's 3.7, a
different major version of exactly the code under suspicion.

## RESOLVED: case 07 — instrument GLOBAL memory only (2026-07-28)

`07_nvshmem_multinode_xfail` hung for a week. Root cause: the backend's
instruction filter was a **blocklist** (skip `NONE`/`CONSTANT`), so it injected
into `LOCAL` (per-thread stack, spilled registers) and `SHARED` (intra-block)
too. Neither can ever be communication, so those records were meaningless *and*
they were the bulk of the work — in one 64-instruction window of
`nvshmemi_transfer_rma_p<int>`, 84 of the accesses were `LDS`/`LDL`/`STL`.

Fix (`3c5d55f`): allowlist `GLOBAL` / `GENERIC` / `GLOBAL_TO_SHARED`. The IBRC
path drops from 47 to ~7.6 injected calls per warp; the NVLink path is
unchanged (`push=18433` before and after), so cases 03/04/06 keep their goldens.
Case 07 now completes on 2 nodes with both providers and captures both
transports in one execution.

**Six theories died on the way, each refuted by measurement, not argument:** the
channel ring-wrap race, a hard 64 MiB channel ceiling, workload volume, PCIe
zero-copy contention (`USE_ASYNC_STREAM` behaves identically), grid/occupancy
(same work in a 4x smaller grid still hangs), and the NVSHMEM barrier kernel
(removing the barriers changes nothing). Two self-inflicted failures also
produced convincing-looking data and are worth remembering: a debug counter read
at recv-thread exit took `g_mutex` while `nvbit_at_ctx_term` held it across
`pthread_join` (wedged teardown at every workload size, invalidated a whole
job), and an earlier "fix" deleted `flush()`'s tail reset while claiming to
harden it (`6f53291` restored it).

## NVBit "deadlock" — case 05 was a TIMEOUT, not a deadlock (2026-07-27)

**Case 05 completes.** Raise its deadline from 180s to 1800s and
NCCL-over-NVLink under NVBit finishes (rc=0, job 43878352) with a valid capture:
`700.pcs` 7440 B on each of 4 ranks, plus peer topology and clock anchors. It
was never deadlocked — NVBit slowdown simply exceeded a 3-minute bound. So the
claim repeated since 2026-07-21, that this is "intrinsic to persistent-kernel
collectives under NVBit, Snoopie has the same limit", is wrong. Case 05 is
promotable to a passing case once it has goldens.

**Case 07 is a different bug and is still open.** It still times out at 1800s
(job 43879008, 30:08) and emits *no gpumem records at all* while ib records
appear. Do not treat 05 and 07 as one failure — that grouping was an inference
from `bytes=0` and it did not survive.

Instrumentation is required to trigger either: NVBit loaded with
`PCOMM_KERNEL_NAME` matching no kernel runs fine; instrumenting the kernel
hangs. So this is our side, not a blanket NVBit/NCCL incompatibility.

## Superseded: the "bounded channel" theory — REFUTED by measurement

Since 2026-07-21 we recorded, in two meta.json files, two plans and this
document, that cases 05/07 hang because *a comm library spins on the device
while NVBit's bounded channel cannot drain*. It was plausible, it matched
Snoopie's documented limits, and it was **never measured**.

Instrumented `recv_thread_fun` (`PCOMM_CHANNEL_DEBUG=1`) and ran case 05:

```
recv_calls=933,389,980   empty=933,389,980   bytes=0   max_recv=0
```

All four contexts, for the entire hang. `ChannelDev::flush()` sets the doorbell
and `recv()` returns >0 exactly when it is set, so: the device never flushed,
the ring never filled, no producer was ever blocked on it, and the recv threads
were healthy the whole time (~19M polls/sec — they were scheduled, just idle).

`bytes=0` also locates it: **the hang precedes any instrumented access being
produced at all**, consistent with the original note that NCCL "inits, prints
version, then hangs". So look at NVBit's interaction with the comm library's
*init* path — instrumentation installation, the CUDA event callbacks under
`g_mutex`, NVBit's lazy module loading — not at data flow. Every fix previously
shortlisted (spillable channel, cooperative flush, IBGDA) targeted the drain
path and would have changed nothing.

**Case 07 shows the identical signature** (job 43877771, measured after the
`heap_regions` race fix): `bytes=0` across all 8 contexts for 290s, still
hanging. So the race was not its cause, and 05/07 are one bug rather than two —
sharing "hangs before producing a byte", not "starves the channel".

Also observed, not the bug but worth knowing: the recv thread busy-polls with no
backoff, burning a core per context.

Aside: `CHANNEL_SIZE` is 1 GiB **per context**, plus a 1 GiB host `malloc` in
each recv thread — 8 GiB of the 4-GPU run is channel buffers that, in case 05,
never receive a byte.

## Provider coexistence & replay (design)
**Finding (2026-07-21) — SUPERSEDED, THE DIAGNOSIS BELOW WAS WRONG.** It read:
*"NVBit takes over the CUDA runtime and interposes `dlsym`; the ibverbs
SmartDlsym interposer also claims `dlsym`. They fight for `dlsym`/init
ownership at constructor time."* Neither half holds. Kept only so the old
conclusion isn't re-derived: it pointed future work at a `dlsym` arbitration
problem that does not exist.

**Corrected root cause + fix (2026-07-27).** Two separate bugs, both fixed.

*Bug 1 — duplicated engine, weak-symbol interposition (the SIGSEGV).* NVBit
never interposes `dlsym`: `nm -D` shows `libnvbit.a` and `libgpumem_trace.so`
only *import* it (`U dlsym@GLIBC_2.34`), while `libibverbs_trace.so` is the sole
definer (`T dlsym`, from pcomm_hook's Dlsym.cpp). The real cause is that both
providers statically embed the header-only engine, so **969 symbols are defined
by both, all weak** — including `guard variable for pcomm::Tracer::instance()::t`,
`rec::ThreadBufferRegistry::instance()::r`, `core::RecordIdTracker::instance()::t`.
The dynamic linker binds every reference to whichever copy loaded first, so the
two providers unknowingly shared one Tracer singleton. gpumem's ctor started a
capture; ibverbs' ctor then called `Tracer::start()` on that same live singleton,
which assigned over `m_recorder` and destroyed gpumem's in-flight Recorder —
SIGSEGV in `~ThreadLocalBuffer<ClockAnchor>::drain()` inside `_dl_init`. Nor was
it "before `main()` in a dlsym fight": the stack is
`_dl_init → pcomm_ibverbs_load → Tracer::start → ~Recorder`.
FIX: `Tracer::start()` is now **attach-if-running** with a refcount — a second
provider joins the running capture instead of re-initialising it (which is the
semantics we want anyway: one process, one recorder, one timeline). `stop()` is
symmetric (last one out finalizes), `atfork_child()` restores the count so a
forked child still flushes, and an `atexit` backstop calls a once-only
`finalize()` so a provider that never calls `stop()` can't strand the capture.

*Bug 2 — `DT_RPATH` leaking the wrong libstdc++ (order-dependence).* The NVHPC
driver bakes `DT_RPATH=[...NVIDIA-HPC-SDK/.../compilers/lib:...GCC/11.4.0/lib64]`
into `libgpumem_trace.so`. `DT_RPATH` outranks `LD_LIBRARY_PATH`, so preloading
gpumem *first* supplied gcc-11.4's libstdc++ process-wide and the gcc-13.2-built
ibverbs provider died on a missing `GLIBCXX_3.4.31/32` — coexistence silently
depended on `LD_PRELOAD` order. FIX: `-Xlinker --enable-new-dtags` emits
`DT_RUNPATH`, which is searched *after* `LD_LIBRARY_PATH`, so the runtime module
(`gcc/13.2.0`, per common.sh) wins and both orders work.

**Reproducer (much cheaper than the old one).** No GPU, no SLURM, no du04 —
on an MN5 login node, with `module load gcc/13.2.0 nvidia-hpc-sdk/25.7`:
`LD_PRELOAD=libibverbs_trace.so:libgpumem_trace.so /bin/true` → was rc=139,
now rc=0 in both orders. `ulimit -c unlimited` gives a core (login nodes block
ptrace, so debug from the core, not a live gdb).

**Verified:** all four load combinations rc=0, libstdc++ resolves to GCC/13.2.0
even with gpumem first, and the full 5-case suite still passes with the shared
tracer change. Co-capture then verified under a real
workload by case 06 (`06_nvshmem_coexist`): both providers write into one
recorder and one rank space, and its NVLink `traffic` golden is byte-identical
to case 04's — i.e. adding `ib` does not perturb what `p2p` captures.

**Shipped (2026-07-23), amended 2026-07-27:** the launcher marks providers
**exclusive** and **replays**. gpumem is **no longer exclusive** — that flag was
set because of the segfault above, whose cause turned out to be fixable, so
`pcomm -p ib,p2p -- ./app` now plans ONE pass (verified: case 06). Replay remains
for genuinely exclusive providers (cupti, when ported). Prefer co-running: replay
re-executes the workload, so its passes are different executions and cannot carry
cross-transport causality. Non-exclusive (hook-based) providers share one pass, each
exclusive provider gets its own pass. Multiple passes ⇒ fork+exec the workload
once per pass, each pass under `out/<label>/`. `pcomm-read` recurses so
`out/ib/rank-0` + `out/gpumem/rank-0` merge as logical rank 0 (disjoint
record-type ranges + per-pass intern tables ⇒ no handle collision). UX: short
aliases (`ib`/`net`, `p2p`/`nvlink`/`snoopie`), `--list`, `--dry-run`. Single
pass still exec's directly (no fork) and writes flat `out/rank-*` (back-compat).

**Still to do:**
- **Replay under MPI is not per-rank fork.** The launcher's fork-a-pass-per-
  exclusive-provider works for a single-process app, but under `mpirun` each MPI
  task *is* `pcomm`; if it forks and the grandchild does `MPI_Init`, PMI wireup
  breaks, and a second `MPI_Init` after pass 1's `MPI_Finalize` is unsupported.
  For MPI jobs, replay must wrap `mpirun` (outer loop: pass 1 for all ranks,
  then pass 2), not fork inside each rank. The integration suite sidesteps this
  by giving every case a single provider *except* case 06, which co-runs both in
  one pass and so sidesteps replay-under-MPI entirely — co-running is now the
  preferred answer to this, not a workaround for it. See tests/hpc/suite/README.md.
- **Wall-align across passes for real.** Passes are re-runs of the same program
  so ClockAnchors make them *alignable*, but the reader doesn't yet fuse the two
  timelines into one Perfetto trace — it merges by rank for the graph/CSV, not
  yet time-overlaid. Thread the anchors through a cross-pass clock origin.
- **Richer conflict model.** Today `exclusive` is a blunt bool → gpumem alone,
  everything else together. cupti (when ported) likely conflicts with gpumem but
  not with ib; that needs the **resource-arbitration** system (model what each
  provider claims — dlsym, the CUDA driver, specific hooks — and compute the
  minimal set of passes + which co-run) rather than one flag.
- **Single master shim owns dlsym + the control plane** (SigmaTrace's
  ProfilerShim). The hook-based providers already share one SmartDlsym per
  process; the launcher is the process-level owner. Formalize when a third
  hook-based provider lands.
- **Replay cost is N full runs + run-to-run nondeterminism** — inherent; warn
  (done) and, later, offer a "record once, attribute many" path where feasible.


## gpumem / NVBit — workload constraints
- **NCCL deadlocks under NVBit** (found 2026-07-23 on MN5 H100) — **a real
  limitation to FIX, not to design away.** NCCL runs a *persistent* collective
  kernel that occupies the GPU for the whole op; NVBit's bounded device→host
  channel fills, and the flush kernel can't launch behind the persistent kernel
  → producer/consumer deadlock (init proceeds, NCCL prints its version, then
  hangs). Intrinsic to instrumenting persistent-kernel collectives with NVBit;
  Snoopie has the same limitation. **Kept as a live known-failing test**
  (`tests/hpc/cases/05_nccl_p2p_xfail`, marked `expect_fail` → run_suite reports
  XFAIL) so the gap stays visible; `03_p2p_kernel` (discrete kernel) is the
  passing stand-in, not a replacement. **Fix ideas:** a spillable/unbounded or
  host-pinned ring channel that doesn't need a device-side flush kernel; or a
  cooperative-launch flush that can preempt/interleave with the persistent
  kernel; or instrument NCCL's kernels with a bounded-buffer backpressure that
  the persistent kernel yields to. Until then, capture NCCL's inter-node story
  on its IB side with the `ib` provider (case 02).
- **The provider allocates in `nvbit_at_ctx_init`** (`cudaMallocManaged` for the
  channel; nvbit_backend.cu:360). NVBit warns this risks deadlock and says to
  allocate in `nvbit_tool_init`. It works for discrete-kernel apps; revisit if a
  discrete workload ever deadlocks at startup (and it may be part of the NCCL fix
  above).
- **Full instrumentation is the firehose; filter to the app kernel.** Unfiltered,
  NVBit instruments *all* device code — for nvshmem that means its internal
  put/spin machinery (~400 MB/rank, asymmetric, non-repeatable). The provider
  honours `PCOMM_KERNEL_NAME=<kernel>` (nvbit_backend.cu:216) to restrict capture
  to the user kernel; case 04 uses it. A production tool wants richer selection
  (module/kernel globs, or an allocation-region filter) rather than one name.
- **NVSHMEM 3.7 device attribution is unreliable** (compounds the 3.x heuristic
  break already noted below): with the kernel filter, case 04's per-access
  `mem_device` comes out as synthetic ids (76–80, not 0–3) and would shift
  run-to-run, so the test asserts the fan-out *count*, not the ids. Fix is the
  host-side ownership path (hook `nvshmem_malloc` / `nvshmem_ptr`).

## Recorder / fork / threading
- **Every record we write contains uninitialised padding bytes.** Verified
  2026-07-30 against a real capture: 2000/2000 records had nonzero garbage in
  `Stamped<GpuAccess>` bytes 1-7 (the padding after `Temporal::kind`), holding
  recognisable stack residue such as `7f38f0b680`. Cause:
  `Temporal::instant()` returns an aggregate whose padding is never
  initialised, and assigning it copies the object representation. Three
  consequences: trace files leak process memory; raw `.pcs` bytes differ across
  otherwise identical runs (this is *why* the determinism gate compares query
  output, not files — self-inflicted, not inherent); and the garbage does not
  compress. Fix by zeroing padding on construction, or as a side effect of
  moving to flatc-generated structs, which zero it.

- **Fork child handler is not async-signal-safe** (`Tracer::atfork_child`, tracer.hpp).
  It allocates (new sink/recorder, `std::start`). Safe for the common
  fork-then-record and fork-then-exec patterns; **unsafe if the process forks
  while another thread holds a malloc/intern/registry lock** — the classic
  fork-in-multithreaded-program hazard. SigmaTrace has the same limitation.
  Circle back if a forking launcher deadlocks: consider prepare/parent handlers
  that acquire/release the relevant locks around fork.
- **Intern tables are inherited on fork, not reset.** A fork child keeps the
  parent's interned handles; its sidecar dump harmlessly over-includes parent
  strings (reader still resolves). Reset per-child if it ever matters.
- **Cross-thread flush assumes quiescent threads at stop().**
  `ThreadBufferRegistry::drain_all()` races a concurrent `record()` on the same
  buffer's count. Fine at shutdown (threads done); revisit if we ever flush
  mid-run while threads are live (would need per-buffer synchronization).

## ibverbs provider
- **The per-record intern is ~87% of tracing overhead.** Tracing costs ~5x
  throughput on small-message ibverbs (`ib_send_bw`, 64 B), and it is not the
  hooks. Measured 2026-07-30 (jobs 43986473, 43986804) by disabling parts of the
  path:

  | variant | ns/msg | attributable to |
  |---|---|---|
  | untraced | ~155 | — |
  | hooks installed, nothing recorded | ~157 | interception ~free |
  | record, but skip `h(cq)` | ~208 | record write ~50 ns |
  | full | ~556-740 | **intern ~350-530 ns** |

  `h(cq)` calls `pcomm_engine_intern_key`, which takes `std::lock_guard` on the
  domain mutex per record to look up a pointer that is virtually always already
  interned. Fix candidates: a thread-local memo of recently-interned pointers
  (hit rate should be near 100% — the same CQ/QP repeat for the whole run), or a
  lock-free read path with the mutex only on insert.

  Caveat on the numbers: run-to-run spread is large (`traced` measured 1.30,
  1.35 and 1.80 Mpps across three runs, i.e. +/-30% on the absolutes). The
  ordering is stable and the intern step is ~2.7x, well outside that, but do not
  quote the nanosecond figures precisely.

  This also reframes the record-format question: at ~50 ns for the actual write,
  FlatBuffers' +25 ns/record is real but small, and the intern is a ~7x larger
  lever than the format choice.

- **~~`post_send` is invisible for apps using the new WR API~~ — FIXED
  2026-07-31, and we now capture sends SigmaTrace structurally cannot.**
  Was: job 43976008, `ib_send_bw` produced 202,000 `ib_poll_cq` and **zero**
  `ib_post_send`, because `ibv_wr_start`/`ibv_wr_send`/`ibv_wr_complete` never
  touch `ctx->ops.post_send`. Case 08 now asserts 1000 sends of 8192 B from
  perftest (job 44032318).

  Three things had to be right, and each was found by measuring rather than
  reasoning:
  1. **The hook point is `ibv_qp_to_qp_ex`, not QP creation.** The wr_* calls
     are `static inline` wrappers over pointers inside `ibv_qp_ex`, so nothing
     is interposable; the pointers must be patched. Creation has many doors —
     `nm -D` on the binary shows perftest uses `mlx5dv_create_qp` and never
     calls `ibv_create_qp_ex`, which is what I hooked first. Every path must
     still come through `ibv_qp_to_qp_ex`, and it is a real exported symbol
     (IBVERBS_1.6), not an inline.
  2. **The interposer needs an entry in `ibverbs.map`.** Its own comment says
     it: "Hiding one does not fail loudly — it silently stops capturing that
     call." Mine was missing, and nothing failed.
  3. **mlx5 re-installs its own `wr_complete` from inside `wr_start`**,
     choosing a per-batch implementation. So the hook installs, `wr_start` and
     `wr_send` stay ours — measured 1000 calls each — and `wr_complete` is the
     provider's every time: every batch built, none ever recorded. It has to be
     re-taken after the provider's `wr_start` returns. A hook that is installed
     and still sees nothing.

  Any "tracing overhead" number taken with perftest *before* this date is
  poll_cq interception only and understates the cost.

- **~~`ibv_reg_mr` is invisible for any app that computes its access flags at
  runtime~~ — FIXED 2026-08-01** (`ibv_reg_mr_iova2` interposed, exported in
  `ibverbs.map`, asserted by case 08's `regions` check). Kept here because the
  mechanism is worth knowing and the same shape will recur. Confirmed against
  verbs.h on MN5 rather than inferred. `ibv_reg_mr` is a macro:

      #define ibv_reg_mr(pd, addr, length, access) \
          __ibv_reg_mr(pd, addr, length, access, \
                       __builtin_constant_p(((int)(access) & IBV_ACCESS_OPTIONAL_RANGE) == 0))

      if (is_access_const && (access & IBV_ACCESS_OPTIONAL_RANGE) == 0)
              return ibv_reg_mr(...);        /* the exported symbol we interpose */
      else
              return ibv_reg_mr_iova2(...);  /* we do not interpose this */

  So a caller lands on `ibv_reg_mr_iova2` in **two** cases: it uses an optional
  access flag (relaxed ordering and friends), *or* its access argument is not a
  compile-time constant. The second is the one that bites — an application that
  builds its flags from options, as perftest does, misses the hook whatever
  flags it ends up with. `ibv_rc_pingpong` passes a literal, which is the only
  reason case 01 sees its MR and has stayed green.

  Measured: case 08's capture has sends and completions and no `100.pcs` at all.
  Fix is the same shape as the WR-API one — interpose `ibv_reg_mr_iova2`, add it
  to `ibverbs.map` (the step that silently did nothing last time), and assert an
  MR count in case 08 derived from what perftest allocates.

  Measured after the fix: one region per rank, 16384 B (2 x the 8192 B message —
  perftest double-buffers its single QP), access 0x100001 =
  LOCAL_WRITE | RELAXED_ORDERING. That relaxed-ordering bit is itself an
  OPTIONAL_RANGE flag, so this registration took the iova2 branch by both routes
  at once.

  The check was verified to fail in the right direction, not just to pass:
  replayed against the pre-fix capture (job 44033938) `regions` goes red while
  `traffic` and `sends_have_payload` stay green, so it isolates this gap rather
  than shadowing the WR-API one.

- **Event timestamps auto-stamp at emit**, not at the hooked call. The hook
  engine already captures precise start/end `Timestamp`s (pre/post); thread
  them into the records (build a `core::Temporal` span) instead of calling
  `record(r)` which stamps `now()` at emit time.
- **Reduced hook set** (phase-1 proof): post_recv, req_notify_cq, post_srq_recv,
  create_cq, alloc_pd(entity), reg_mr_iova2, reg_dmabuf_mr, create_ah,
  get_device_guid, query_device are defined but not all emit records. Expand as
  needed.
- **dereg_mr / destroy_qp** are interposed but emit no lifetime-close record yet
  (entities are open-ended). Add close events when lifetime queries matter.

## gpumem / NVSHMEM
- ~~**NVSHMEM 3.x ownership heuristic breaks on NVSHMEM 3.7**~~ and ~~**the 2.x
  legacy path**~~ — both deleted 2026-08-03. The positional model and the
  hardcoded-VA model are gone; ownership comes from `nvshmem_ptr` +
  `cuPointerGetAttribute`, which read no version and assume no layout, so "which
  NVSHMEM fragments the heap" stopped being a question anyone has to answer.
  See the FIXED entry at the top.
- **Team-scoped symmetric allocations are unhandled.** `nvshmemx_mc_ptr` /
  multicast addresses are not `nvshmem_ptr`-resolvable; they fall through to the
  alloc registry, resolve to `run_device`, and are filtered out — i.e. missing,
  not wrong. No case uses them today.
- **gpumem provider not re-validated on du04 after the fork/cross-thread tracer
  changes** — only core tests + the ibverbs provider were re-checked. Rebuild
  libgpumem_trace.so on du04 and re-run the diim/nvshmem baselines before
  relying on it.

## Reader / export
- **NCCL rank→LID reconstruction is approximate.** The rank-level comm graph
  derives each rank's LID from `lid[dest_qp_num]=dest_lid` over RTR rows, which
  is incomplete when a rank's QPs never appear as a `dest` (some ranks resolved
  to a shared LID in the NCCL run). Good enough for the node-level graph; for a
  clean rank-level graph, capture the local LID directly (hook `ibv_query_port`
  or read it at `modify_qp`).
- **Perfetto `.pftrace` is minimal.** No interned strings or `clock_snapshot`
  packets — fine at 8 ranks / 96k events, but interning names + a proper clock
  domain would shrink large traces and give absolute time.
- **`--perfetto`/`--pftrace` share ~60 lines of clock-calibration + track
  setup.** Factor out when a third exporter appears.

## Launcher / rank assignment
- **RankDetector trusts `SLURM_PROCID`, which is per-srun-step.** Launching N
  processes as separate `srun --ntasks=1` steps gives every one `SLURM_PROCID=0`
  → they collide into `rank-0` in a shared output dir. Workaround: set
  `PCOMM_RANK` explicitly per process (see tests/hpc/ib_ring.sbatch). Proper
  fix: the `pcomm` launcher should own rank assignment rather than inferring it
  from an env var whose scope depends on how the job was launched. (The
  LID-keyed comm graph reconstructs regardless, but per-rank streams are lost.)

## Infra / process
- **BLOCKER (2026-07-30), I caused it: `build-gpumem` cannot be reconfigured.**
  Diagnosed, not fixed. The SDK ships **two** nvcc binaries:

      $SDK/compilers/bin/nvcc  -> CUDA 11.8  (no c++20)   <- what PATH resolves
      $SDK/cuda/bin/nvcc       -> CUDA 12.9               <- what we need

  where `SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7`. Every build up to
  today reused a `build-gpumem/CMakeCache.txt` created in an earlier session
  that had the 12.9 compiler recorded; `run_on_mn5.sh` runs `cmake -B
  build-gpumem` which *reuses* an existing cache, so nobody noticed PATH points
  at 11.8. **I destroyed that cache with `rm -rf build-gpumem`**, and a fresh
  configure now picks 11.8 and dies on `-std=c++20`.

  Forcing `-DCMAKE_CUDA_COMPILER=$SDK/cuda/bin/nvcc` gets past that but then
  fails CMake's compiler test at link (`collect2: error: ld returned 1 exit
  status`) — the 12.9 nvcc presumably needs matching library paths that
  `module load nvidia-hpc-sdk/25.7` does not set, since the module is oriented
  at the `compilers/` toolchain. See
  `build-gpumem/CMakeFiles/CMakeError.log` for the link line.

  Next steps: check for a separate `cuda/12.9` module; compare `LD_LIBRARY_PATH`
  and `LIBRARY_PATH` against what a working build had; or set
  `CMAKE_CUDA_HOST_LINK_LAUNCHER`/library dirs explicitly. Once it builds, pin
  the compiler in `run_on_mn5.sh` so a cache wipe can never silently downgrade
  the toolchain again.
- **The CUDA target does not track header dependencies.** Changing
  `libpcomm/include/**` does NOT rebuild `gpumem_trace`; make reports
  "Built target gpumem_trace" and relinks nothing. `run_on_mn5.sh` then `cp`s
  the stale `.so` into `build/providers/gpumem/`, where its fresh mtime makes it
  look current. **Consequence: a green suite can be reporting on a gpumem binary
  from a previous day.** Verified 2026-07-30 — the padding fix (449fb3b) was
  absent from the .so used in the suite run that "confirmed" it, and the fresh
  capture still showed 5000/5000 records with dirty padding. Always check
  `build-gpumem/providers/gpumem/libgpumem_trace.so`'s own mtime, not the copy.
  Fix: make the CUDA target depend on the libpcomm headers, or have
  `run_on_mn5.sh` force a gpumem rebuild.

- **Memory key/host split.** Canonical pcomm memories live on
  `mobile-base:~/.claude/.../-home-mktips-pockt-uni-lab/memory`. This session
  runs under the `-home-mktips-pockt-uni-lab-pcomm` key on university-base, and
  mobile-base was offline — recent progress is stashed in this machine's project
  memory (`pcomm-phase1-ibverbs-pending-sync.md`) with a reconcile note. Merge
  it into `pcomm-phase1-decisions.md` on mobile-base and delete the stash when
  that host is back.
- **Local dev box (university-base) is macOS-flavored** (BSD sed, clang, no
  ELF/`link.h`, no `CLOCK_MONOTONIC_RAW`): libpcomm's hook engine and recorder
  build only on Linux targets (du04 / MN5). Use `perl -i` not `sed -i` locally;
  build/verify remotely.

## OPEN (2026-08-05): case 08 drives a queue pair nothing recorded creating
5 `ib_modify_qp` rows exist for queue pairs that neither `ib_queue_pair` nor
`mlx5_queue_pair` records, while the ib provider is healthy in the same process
(1000 post_send, 1010 poll_cq). The standing explanation — "perftest uses
`mlx5dv_create_qp`, which we don't hook" — is **refuted**: the hook exists,
emits an entity, and captured nothing. Versioning, the RDMA CM route, DEVX,
provider-load failure and id-range conflict are all eliminated by measurement
(probe job 44205459). See `PLAN-20260805-0053-perftest-qp-creation-todo.md`.

**Method note worth keeping.** `LD_DEBUG=bindings` shows symbol *resolution*,
not *invocation* — `rdma_create_qp` and `rdma_create_qp_ex` both bound in a run
where perftest can call only one. It proves "if called, it reaches us", which is
what killed the versioning hypothesis, and cannot name the caller. Three
successive explanations here were asserted from resolution-level evidence and
all three were wrong; the next step is call-level instrumentation.

## FIXED (2026-08-04): a case that declined to run reported PASS
MN5's board showed 9 PASS where the ninth was case 09 skipping in 0s because the
host has no IBGDA. `result.json` said `skipped:true` and the board rendered
`PASS` anyway — nine greens implying coverage that does not exist. Skips now
render `SKIP` with their reason and the banner names the count.

Two smaller reporting defects went with it. Case-level `expect_fail` keyed on an
`outcome` field **nothing writes**, so the XFAIL branch fired unconditionally
and **XPASS was unreachable** — a case marked xfail that started working would
have reported XFAIL forever, which is the one signal the marker exists to
produce. And assertion-level xfails rode invisibly inside a PASS (case 08 passes
with one known-failing assertion; the board said only "PASS 10 checks").

## FIXED (2026-08-04): a broken harness reported a green board
Case 09's first run on du04 returned `ok:true` having tested nothing: the
provider built, all three IBGDA prerequisites passed, then `nvcc` was missing
because `common.sh` hardcoded MareNostrum's SDK path — and `skip()` swallowed
it. SKIP now means only "this host cannot do IBGDA"; a host that can, where the
harness broke, FAILS. `common.sh` resolves the SDK by search with env override,
globs `comm_libs/*/` for the CUDA version, and locates `nvcc` explicitly because
`module` is a MareNostrum thing that silently no-ops elsewhere.

## OPEN (2026-08-05): run_on_a100.sh syncs from a syncthing-fed checkout
A board can execute a **mixed tree**: the provider `.cpp` and the `.sql`
assertion beside it in the same commit arrived separately, so a fixed provider
was judged by a stale assertion and went red for a reason that no longer
existed. `run_on_mn5.sh` cannot do this — it pushes HEAD over git and refuses a
dirty tree. The A100 runner now prints the source commit, flags a dirty tree and
honours `PCOMM_EXPECT_COMMIT`, but the underlying hop is still asynchronous:
pin the commit when a board needs to be attributable.

## FIXED (2026-08-05): a queue pair driven by a door that was never a symbol
Case 08 recorded five `ib_modify_qp` rows against a queue pair no record had
created, and six hypotheses died to `nm`, `readelf` and `LD_DEBUG=bindings`
before the seventh was even reachable. All three answer questions about
**symbols**; `ibv_create_qp_ex` is a `static inline` dispatching through
`verbs_context::create_qp_ex`, so none of them could name it and each failure
read as "wrong guess" rather than "wrong category of thing". Settled by tracing
hook **entry** (`PCOMM_TRACE_HOOKS=1`) at the engine choke point, which covers
every hook in every provider at once — the *absence* across all of them was the
evidence. Hooked as an object-ops slot, reproducing the two guards
`verbs_get_ctx_op` applies, because patching a NULL slot would advertise a
capability the application had declined.

## FIXED (2026-08-05): device attribution asked CUDA for the wrong attribute
`kAttrDeviceOrdinal` was 4 — `CU_POINTER_ATTRIBUTE_HOST_POINTER`, not
`DEVICE_ORDINAL` (9). Every GPU buffer the mlx5dv provider ever saw came back
`kUnknownDevice`, since the provider was written. It survived because
`gpu_driven` ORs in `binding == ByDmabuf`: under DMABUF the answer came out
right without consulting CUDA at all, so case 09's engagement gate was
effectively asserting the binding rather than GPU residency — circular with
`umem_binding_as_configured`, and **unfalsifiable by any by_dmabuf case**. Only
case 10's by_addr binding could expose it.

Two general points, both of which have now cost time twice:
- **Hand-copied constants have no compiler behind them.** This provider does not
  link CUDA on purpose, so nothing checks the `kAttr*` values.
- **A fallback that makes the common case right hides the general case being
  wrong.** Worth auditing the other discriminators for the same shape.

## FIXED (2026-08-06): `gpu_driven` under dmabuf was a heuristic
Under by_dmabuf the umem address is 0, so there was nothing to ask CUDA about
and `gpu_driven` fell back to `binding == ByDmabuf`. Case 09's `ibgda_engaged`
gate was therefore asserting the binding — which `umem_binding_as_configured`
already asserts — and was unfalsifiable by any dmabuf case. That is *why* case
10 had to exist before the wrong-CUDA-attribute bug could surface at all.

Closed by intercepting the export: NVSHMEM converts a device pointer to a
descriptor with `cuMemGetHandleForAddressRange(&fd, dptr, size, DMA_BUF_FD, 0)`
before registering, so catching that call yields `fd → (dptr, size)`. Both
resolution doors are hooked, because `LOAD_SYM` picks between
`cuGetProcAddress` and a `dlsym` fallback at runtime
(nvshmem-3.7.2 `cudawrap.cpp:38-45`). Verified on du04: `mlx5_umem.device`
65535 → 0, `mem_object` populated with real GPU addresses, and `addr` correctly
still `0x0`. The fallback is gone; `gpu_driven` now means GPU-resident, and an
unobserved export reports false rather than assuming true.

Two things NOT closed by it, both recorded in `cuda_dmabuf.hpp`:
- **fd reuse.** A descriptor is unique only while open, exactly like an address.
  A stale hit is possible in principle and has not been observed; the durable
  fix is to key on the dmabuf inode (`fstat`), which is unverified.
- **The registry is per-`.so`.** It lives in mlx5dv because that is its only
  consumer. Sharing it across providers needs the engine — a static library
  linked into two `.so`s gives two copies, not one.

## NVBit: instrumenting a 64-bit-indexed global access raises an illegal memory access

**Status:** root-caused 2026-08-10, worked around in our fixture, NOT fixed.
**Impact:** any application indexing a buffer with a 64-bit expression — i.e. any
buffer over 2 GB — cannot be captured with `-p p2p`. It is not a subtle
mis-attribution; the context is poisoned and the capture is lost.

Reproduced on MN5, 4 PEs, one node, NVSHMEM 26.5, sm_90. A kernel doing
`nvshmem_int_p(&dst[idx], src[i], peer)` faults under instrumentation when `idx`
is 64-bit and does not when it is 32-bit. Same source, same launch, same kernel
name, one word changed:

| kernel | result |
|---|---|
| `long i, long dst_off` | FAULT |
| `int i, long dst_off` (`dst_off + i` promotes) | FAULT |
| `long i`, no offset | FAULT |
| `int i`, no offset | clean |
| `int i, int dst_off` | clean |

Evidence it is the instrumentation and not the workload:

- The workload runs clean with no profiler (job 44458142, exit 0, all PEs
  correct).
- `PCOMM_INSTR_LO/HI` bisect (44464306, 44464689): each of the kernel's three
  instrumented instructions faults on its own —
  `LDG.E.64.CONSTANT`, `LDG.E`, `ST.E` — while all 1485 instructions in
  `nvshmemi_transfer_rma_p<int>` instrument cleanly. Not an instruction class,
  not an address space.
- The NVSHMEM version is exonerated: case 04's workload rebuilt against the
  current SDK still passes (44464083).

Case 04 has always passed for one reason only: `put_to_next` uses `int i` and
`&dst[i]`.

**Next step if picked up:** dump the SASS of a faulting and a clean variant
(`nvdisasm` on the cubin) and compare what NVBit injects around the access. The
64-bit form uses `IMAD.WIDE` and a register pair for the address; the suspicion
is the injected call's handling of that pair. Same family as `3c5d55f`, where a
`LDL.64` in the same NVSHMEM function broke instrumentation and was excluded
rather than fixed.

