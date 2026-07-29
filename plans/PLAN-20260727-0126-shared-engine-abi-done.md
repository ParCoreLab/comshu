# PLAN — shared engine ABI (one engine per process, N plugins)

- **Created:** 2026-07-27 01:26 +03
- **Status:** done (2026-07-29)
- **Ready to start.** Its prerequisite (case 06, the coexistence regression
  test) landed in `584fc2a`. One caveat carried into "Sequencing" below: the
  cross-transport *traffic* half of the safety net is still missing, blocked on
  the NVBit multinode deadlock (`07_nvshmem_multinode_xfail`).
- **Context commits:** `7a5b55a` (coexistence fixes), `fc8ccd6` (suite verdict
  pinning), `584fc2a` (cases 06 + 07)

## Why this exists

Coexistence works as of `7a5b55a`, but it works *by accident of the linker*.
Both providers statically embed the header-only engine, so **969 symbols are
defined by both, all weak** — including the singleton guards:

```
guard variable for pcomm::Tracer::instance()::t
guard variable for pcomm::rec::ThreadBufferRegistry::instance()::r
guard variable for pcomm::core::RecordIdTracker::instance()::t
```

The dynamic linker binds every reference to whichever copy loaded first, so the
two providers share one Tracer *unintentionally*. Worse, those copies are built
by **different compilers** — gcc-11.4 host for the CUDA/NVBit side, gcc-13.2 for
ibverbs — and they pass `std::map` / `std::string` / `unique_ptr` across that
boundary. libstdc++'s ABI stability makes it work today. It is load-bearing
accident, not design, and it will not survive more plugins.

## What must be singular vs. what stays inline

The engine is header-only because of the hot path, and that reason survives.

| Must be exactly one per process | Can stay inline per provider |
|---|---|
| Tracer lifecycle, Recorder, sink/file IO | encoding a record `R` into bytes |
| intern tables, `ThreadBufferRegistry`, `RecordIdTracker` | the 4096-slot `ThreadLocalBuffer` append |
| `dlsym` (only one `.so` may define it) | |

So the ABI crossing is *acquire a buffer / flush a buffer / intern a string* —
**not per-record**. `IDrainable` is already a virtual interface and is the
natural seam. That is what makes a narrow boundary viable without wrecking
throughput.

## Options considered

**A. `libpcomm_engine.so` with a C ABI** — `pcomm_engine_acquire/release`,
`pcomm_engine_buffer(type_id, elem_size)`, `pcomm_engine_flush(arena*)`,
`pcomm_engine_intern(domain, ptr, len)`. Providers link only a C header; the
templated `record<R>()` becomes an inline wrapper over it.

**B. `libpcomm_engine.so` exporting C++** — smallest diff: flip the CMake target
to SHARED, add visibility control.

**C. Keep duplication, isolate with `-fvisibility=hidden` + version scripts** —
each provider gets a private engine; no interposition, no crash.

**C is disqualified as the primary.** N private engines ⇒ N recorders, N intern
tables, N output streams in one process. That is replay-inside-one-process, and
it destroys the single unified timeline that is the entire reason to make them
coexist. (Still do the visibility hardening — see step 3 — just not *instead* of
a shared engine.)

**B vs A turns on one fact: the mixed toolchain is permanent.** NVBit forces a
gcc-11.4 host compiler; ibverbs needs gcc-13.2 for `GLIBCXX_3.4.31`. B leaves
gcc-11.4 code passing C++ containers to a gcc-13.2 engine — the same
load-bearing accident, merely centralized. Future plugins make it worse: CUPTI
ships its own headers, UCX/libfabric would likely come from site modules with
their own compilers.

**DECISION: A.** A C ABI makes the compiler question disappear permanently.
`pcomm_abi_version()` already exists as the versioning hook — it just needs
teeth (check at acquire, refuse on mismatch).

## What the codebase already gets right (do not re-invent)

- `provider/provider.hpp` already declares `IProvider`, `ProviderInfo{name,
  id_range_lo, id_range_hi}`, `pcomm_create_provider()`, `pcomm_abi_version()`.
  **All of it is currently unconsumed** — nothing dlopens a provider; both
  providers self-start from ELF ctors (`pcomm_ibverbs_load`) or vendor hooks
  (`nvbit_at_init`). This work is largely *wiring up a boundary already spec'd*.
- Per-thread buffers are keyed `{recorder_ptr, type_id}` (`recorder.hpp:75`), so
  with one shared Recorder the range-allocated ids make provider streams
  disjoint **by construction**. Ranges live in `core/ids.hpp`: ibverbs 100-199,
  mlx5dv 200-299, cupti 300-399, libfabric 400-499, internal 500-599,
  ucx 600-699, gpumem 700-799, ucx_deep 800-899.

## Steps

1. **[done]** `Tracer::start()` = attach-if-running, refcounted; symmetric
   `stop()`; `atfork_child()` count restore; `atexit` → once-only `finalize()`.
   Commit `7a5b55a`.

2. **[done — 2a] `libpcomm_engine.so` exists and owns the singletons.** Commit
   `d5675e2`. `Tracer`, `Recorder<DefaultConfig>`, intern tables,
   `ThreadBufferRegistry`, `RecordIdTracker` and `Dlsym.cpp` all moved in;
   `pcomm_hook` went STATIC -> SHARED. Guard variables co-defined by both
   providers: **3 -> 0** (total co-defined symbols 969 -> 771, but the rest are
   pure functions/vtables — harmless vague linkage).

   **[done — 2b] The C ABI itself.** Commit `4dd6a69`. Providers still call C++ methods across the
   `.so` boundary, so the cross-compiler hazard that motivated choosing A over B
   is *centralized, not removed*: gpumem is still built by nvcc/gcc-11.4 and now
   calls into a gcc-13.2-built engine. Landed as
   `pcomm/engine_abi.h`: records append into a POD arena
   (`pcomm_engine_arena`/`_flush`) cached in a thread_local, so the per-record
   `std::map` lookup is gone too. gpumem and ibverbs now import **only**
   `pcomm_engine_*` from the engine; co-defined symbols 771 -> 160.

   Which crossing actually mattered was narrower than assumed:
   `RegisteredSymbols` is ibverbs-only and same-compiler, so the real hazard was
   `buffer_store()` — gcc-11.4 doing `operator[]` on a gcc-13.2 `std::map`, once
   per record. Also had to *reverse* 2a's `extern template`: correct while the
   Recorder owned state, but afterwards it made providers import
   intern/drain_thread as mangled C++.

   **REMAINING GAP:** hook providers still reach
   `smart_dlsym::register_symbol`, which manipulates a
   `std::unordered_map<std::string, std::function>` inline. ibverbs only today,
   same compiler as the engine, so safe *now* — a hazard the moment a hook
   provider is built by another toolchain. Give it a C shim
   (`pcomm_engine_register_symbol(name, fn_ptr, user_data)`) when the second
   hook provider (UCX/libfabric) lands, or sooner if convenient.

   **HARD-WON CONSTRAINT for 2b — the engine must be PRELOADED, not just
   linked.** The linker searches preloaded objects before the executable's own
   dependencies. With `dlsym` in a mere `DT_NEEDED`, libc's `dlsym` wins, the
   SmartDlsym interposer never runs, and everything the target resolves at
   runtime goes uncaptured. It cost 12 of 16 memory regions and 36 of 432 queue
   pairs in case 06 — silently, no crash, no error. The launcher now puts the
   engine first in every LD_PRELOAD chain (self-located via `dladdr`, override
   with `PCOMM_ENGINE_PATH`). Do not undo that in 2b.

3. **[done — 2c8515b] Version scripts on every provider.** ibverbs 2536 -> 9
   exports, gpumem 3438 -> 9. `-fvisibility=hidden` turned out unnecessary: an
   explicit version script already reduces the dynamic table to the listed
   names, and it is the safer of the two because the export set is written down
   rather than inferred. Startup assertion landed as
   `pcomm_engine_interposer_live()`, verified in BOTH directions (job 43972895):
   a deliberately mis-launched run — provider preloaded with the engine only as
   a DT_NEEDED — does trigger it. A check that cannot fire reads as reassurance.
   nvc++ needs raw `LINK_FLAGS`; see the CMakeLists.

   ~~[todo] `-fvisibility=hidden` + version scripts on every provider.~~
   Also add here: **a startup assertion that the interposer is actually live.**
   Every bug in this refactor failed by producing a plausible green rather than
   an error (stale captures, the case 07 label mismatch, the dlsym loss). Only a
   golden with enough entities in it noticed the last one; the build and runtime
   said nothing. The engine should verify at acquire that its `dlsym` is the one
   a caller resolves, and complain loudly if not. Today
   ibverbs exports 3831 dynamic symbols and gpumem 4482; a provider needs
   roughly `pcomm_create_provider`, `pcomm_abi_version`, and its interposers.
   That export surface *is* the bug class of `7a5b55a`. Cheap defense-in-depth
   even once the engine is shared.

4. **[done — 957939a] Enforce at acquire:** `pcomm_engine_declare_provider`
   checks ABI version and rejects overlapping id ranges; `pcomm_engine_arena`
   warns once per record type outside every declared range. The engine declares
   its own internal range (500-599) rather than the check special-casing it —
   a consistency check with a hidden exception is one you stop trusting. ABI
   bumped 1 -> 2.

   ~~[todo] Enforce at acquire:~~ ABI version match; reject overlapping
   `id_range` claims; flag records emitted outside a provider's declared range.
   Disjointness is what lets N plugins merge into one stream and `pcomm-read`
   already assumes it — nothing checks today.

## Design constraints to honour

- **`dlsym` ownership.** Only one `.so` may define it; today that is ibverbs via
  `pcomm_hook`. A second hook provider (UCX, libfabric) reintroduces duplicate
  definitions. `Dlsym.cpp` moves into the engine; providers *register symbol
  interest*. This is the old notes' "single master shim" idea — but for the
  correct reason (one definition of `dlsym`), **not** the debunked one (NVBit
  does not interpose `dlsym`; it only imports it). `Dlsym.hpp` already says
  *"This library can only be linked against ONCE in the entire program"* — the
  code knew, the build never enforced it.

- **Two provider load-classes must both work.** Preload/hook providers (ibverbs,
  future UCX) come up in their ELF ctor; vendor-runtime providers (gpumem/NVBit,
  future CUPTI) come up in `nvbit_at_init` / CUPTI callbacks. Neither can be
  forced into the other's model — but both can just `acquire`/`release`. The
  refcount from step 1 gets promoted from a Tracer implementation detail to the
  engine's **public contract**.

- **Don't regress the hot path.** `record()` currently does a `std::map` lookup
  per record on `{recorder, type_id}` — already non-trivial cost. Cache the
  arena pointer in a provider-side `thread_local`; that likely makes the C
  boundary a net win rather than a tax. Measure before/after.

- **Keep the `DT_RUNPATH` fix.** `providers/gpumem/CMakeLists.txt` passes
  `-Xlinker --enable-new-dtags` so the NVHPC-baked rpath to gcc-11.4's libstdc++
  can't outrank `LD_LIBRARY_PATH`. Any new provider built by a vendor driver
  needs the same treatment.

## Sequencing note (updated 2026-07-27, after case 06 landed)

Case 06 (`06_nvshmem_coexist`) is **in and passing** — case 04's workload with
both providers in one process, goldens verified across two independent jobs. So
step 2 now has a net for the *coexistence* property: if the refactor
reintroduces symbol duplication or breaks the shared Tracer, case 06 fails and
no other case would notice.

**[RESOLVED 2026-07-29 — the net is complete.]** Case 07 is green and asserts
cross-transport traffic in one execution, so steps 3-4 had the full safety net
that step 2 went in without. Historical text follows.

**But half the net is missing.** The unified NVLink+IB *traffic* correlation has
no regression test, because the 2-node case deadlocks under NVBit
(`07_nvshmem_multinode_xfail`, see things-to-circle-back-to.md). Single-node
NVSHMEM never posts an IB send, so case 06 proves both providers write into one
capture but not that cross-transport traffic lines up. Going into step 2 with
that gap open is a deliberate, known risk — not an oversight.

If the NVBit channel work (spillable/host-pinned channel) happens first, case 07
turns green and the net is complete. Otherwise: accept the gap, and be extra
careful that step 2 does not change record-type-range handling or rank-space
merging, since those are exactly what an unverified cross-transport capture
depends on.

## Cheap reproducer to keep using

No GPU, no SLURM, no du04. On an MN5 login node with
`module load gcc/13.2.0 nvidia-hpc-sdk/25.7`:

```bash
LD_PRELOAD=libibverbs_trace.so:libgpumem_trace.so /bin/true   # rc must be 0, both orders
nm -D --defined-only libibverbs_trace.so | grep -w dlsym      # exactly one definer
readelf -d libgpumem_trace.so | grep -E 'RPATH|RUNPATH'       # must be RUNPATH
```

Login nodes block ptrace, so debug from a core (`ulimit -c unlimited`), not a
live gdb.

## Open question

Whether the engine `.so` should also absorb `pcomm-read`'s decode side so the
record layout has exactly one definition across writer and reader. Currently the
reader re-declares the layouts. Not urgent, but it is the same class of
duplication that caused `7a5b55a`.
