# pcomm — things to circle back to

Known limitations / deferred work, recorded so they aren't lost. Newest first.

## Provider coexistence & replay (design)
**Finding (2026-07-21):** naive dual-`LD_PRELOAD` of gpumem (NVBit) + ibverbs
**segfaults during program startup, before `main()`** — a load-time collision.
NVBit takes over the CUDA runtime and interposes `dlsym`; the ibverbs
SmartDlsym interposer also claims `dlsym` and brings up the Tracer. They fight
for `dlsym`/init ownership at constructor time. gpumem-alone and ibverbs-alone
both work. Reproduce on du04: `LD_PRELOAD=libgpumem_trace.so:libibverbs_trace.so
./diim` → SIGSEGV at startup (need the ibverbs provider built there; verbs.h
staged from MN5 under `~/ibhdr`).

**Direction (do NOT deep-fix the co-run):**
- Mark providers **exclusive** = "must take full control of the runtime" (NVBit
  gpumem is one; NVBit tools can't share a process). Non-exclusive providers
  (the hook-based network ones — ibverbs/mlx5dv/ucx/libfabric) *can* share one
  process because they route through the single `pcomm::hook` / SmartDlsym
  interposer (one dlsym owner).
- The launcher runs all compatible (non-exclusive + at most one exclusive?)
  providers in one process; for any combination that isn't co-runnable, use a
  **replay mechanism**: re-exec the same workload once per exclusive provider
  and merge the per-run captures. The runs are alignable (same program, wall-ns
  clock anchors), so the reader stitches them into one graph — at the cost of N
  runs and run-to-run nondeterminism.
- **Future:** a resource-arbitration system that models what each provider
  claims (dlsym, the CUDA driver, specific hooks) and computes the minimal set
  of runs + which providers co-run, instead of a blunt exclusive flag.
- Prereq either way: a **single master shim owns dlsym + the control plane**
  (SigmaTrace's ProfilerShim; pcomm's launcher is meant to be this) so
  hook-based providers coordinate through one interposer.


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
