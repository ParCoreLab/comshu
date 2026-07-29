# pcomm — things to circle back to

Known limitations / deferred work, recorded so they aren't lost. Newest first.

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
NVSHMEM's proxy path should hit this. **Snoopie instruments the same way** (our
`inject_funcs.cu` is a port of its `instrument_mem`, and its filter is the same
NONE/CONSTANT blocklist), so its documented "multinode NVSHMEM doesn't work"
limitation is plausibly this bug rather than anything intrinsic.

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
- **NVSHMEM 3.x ownership heuristic breaks on NVSHMEM 3.7** (validated). 3.7
  fragments the symmetric heap into ~788 cuMemMap regions, so the "one region
  per PE" positional model mis-attributes. Real 3.7 layout is a fixed-VA
  windowed scheme (base 0x10020000000, stride 0x4000000000). Fix options:
  detect the npes evenly-strided super-windows, or move to host-side ownership
  (hook `nvshmem_malloc` / query `nvshmem_ptr`) — the robust, version-proof
  path. See memory `pcomm-nvshmem-version-fragility` / `-hostside-ownership`.
- **NVSHMEM 2.x legacy path** (`find_nvshmem_dev_of_ptr_legacy`) coded but never
  exercised (du04 has 3.x only).
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
