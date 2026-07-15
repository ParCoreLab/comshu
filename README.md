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
    gpu/         IGpuBackend HAL (AMD-ready bridge)
providers/       L3 — each builds one .so (vendor types live HERE, not in core)
  gpumem/        GPU memory-access provider (Snoopie engine)
  ibverbs/       proof-of-concept network provider (from SigmaTrace)
gpu-backends/    vendor impls behind gpu/IGpuBackend
  nvbit/         phase-1 default (confirmed working on du04: CUDA 13.0, driver 580)
  sanitizer/     Compute-Sanitizer (stub; phase 2)
pcomm/           launcher binary
pcomm-read/      reader binary (binary streams -> Parquet/Perfetto)
tests/           core unit/smoke tests (no GPU required)
```

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
