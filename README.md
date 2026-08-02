# pcomm — ParCore Lab Communication Profiler

Unified GPU-communication profiler merging **SigmaTrace**, **Snoopie**, snoopie-sanitized,
and (later) ucTrace. Design docs live one level up: [`../UNIFICATION.md`](../UNIFICATION.md)
(what/why) and [`../ARCHITECTURE.md`](../ARCHITECTURE.md) (layers, interfaces, patterns).

| Piece | Name |
|---|---|
| Core library (L0–L2) | `libpcomm` |
| Launcher | `pcomm` |
| Post-processor / reader | `pcomm-read` |
| Namespace | `pcomm::` (`core`, `rec`, `hook`, `attr`, `gpu`, `provider`) |
| Env prefix | `PCOMM_*` |

## Layout

```
libpcomm/        L0–L2 core — vendor-free (no cuda*/hip*/ibv_*/Sanitizer_* types allowed)
  include/pcomm/
    core/        record model (Event/Entity/Artifact/Edge), time, ids, neutral vocabulary
    rec/         Recorder, ThreadLocalBuffer, InternTable, sinks (the hot path)
    hook/        HookTrait / CompositeHookManager        [ported from SigmaTrace/Hook]
    attr/        Collector/Filter/Attributor pipeline    [from snoopie-sanitized design]
providers/       L3 — each builds one .so (vendor types live HERE, not in core)
  gpumem/        GPU memory-access provider (Snoopie engine)
  ibverbs/       proof-of-concept network provider (from SigmaTrace)
gpu-backends/    vendor capture engines feeding the gpumem provider
  nvbit/         phase-1 default (confirmed working on du04: CUDA 13.0, driver 580)
  sanitizer/     Compute-Sanitizer (stub; phase 2)
pcomm/           launcher binary
pcomm-read/      reader binary (binary streams -> Parquet/Perfetto)
tests/           core unit/smoke tests (no GPU required)
```

## Usage

Pick what to capture by short name; `pcomm` assembles the run and reads it back.

```bash
pcomm --list                       # what can I capture, and how?
pcomm -p ib      -- ./app args     # InfiniBand wire traffic (inter-node)
pcomm -p p2p     -- ./app args     # GPU-initiated NVLink peer r/w (intra-node, Snoopie)
pcomm -p ib,p2p  -- ./app args     # both — see "replay" below
pcomm-read pcomm-out               # inspect / export the capture
```

Providers are chosen with `-p name[,name...]` (each accepts short aliases, e.g.
`ib`/`net`, `p2p`/`nvlink`/`snoopie`). Add `--dry-run` to print the plan.

**Replay.** Some engines must own the whole runtime — NVBit (`p2p`) takes over
the CUDA runtime and `dlsym`, so it can't share a process with anything else.
When a selection mixes such an *exclusive* provider with others, one process
can't host them, so `pcomm` **replays**: it runs the workload once per exclusive
provider and drops each pass under one `-o` directory (`out/ib/`, `out/gpumem/`,
…). The passes are wall-clock-aligned, and `pcomm-read` stitches them back into
one per-rank picture (each provider owns a disjoint record-type range, so a
rank's network and NVLink traffic merge cleanly). Hook-based providers (all the
network ones) share a single pass — no replay needed among them.

## Phase 1 scope (decided 2026-07-15)

1. New four-archetype record model with interning, from day one.
2. gpumem provider with the **NVBit** backend first (Sanitizer stubbed).
3. **ibverbs** as the single network proof provider; the rest of SigmaTrace's
   providers follow in phase 2. ucTrace deferred.
4. Test bed: `ssh a100` (du04, 4x A100). Multinode validation deferred until
   supercomputer access.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Core + tests build with no CUDA/verbs installed; providers are enabled
individually (`-DPCOMM_PROVIDER_GPUMEM=ON`, `-DPCOMM_PROVIDER_IBVERBS=ON`) and
require their vendor SDKs.
