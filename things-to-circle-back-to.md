# pcomm — things to circle back to

Known limitations / deferred work, recorded so they aren't lost. Newest first.

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

Related: case 06's `regions_per_rank` (115 -> 114) and `ib_entities`
(mem_region 460 -> 456) fail *independently of any of this* — they fail at
`5a28119` too. Probably the same family (allocation-dependent goldens), not yet
diagnosed.

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
