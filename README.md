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

## Gates

There is no hosted CI (the repo is mirrored between machines, not on a forge),
so the gates are local and named:

- **Every commit**: `tools/hooks/pre-commit` (install once with
  `tools/install_hooks.sh`) builds the core configuration and runs its tests,
  including `tools/schema_check.py`. Skippable with `--no-verify`; the MN5 gate
  is what catches you.
- **Anything touching capture semantics** (hook engine, records, providers,
  stream format): `tests/hpc/run_on_mn5.sh` — sync, build both providers on
  MareNostrum 5, run the 8-case suite, compare SQL goldens. This is the merge
  gate; a change is not done until this board is green.

## Adding a record type

One entry per layer; the build refuses a missing one:

1. `PCOMM_RECORD` in `core/records.hpp` (or the provider's `*_records.hpp`),
   type_id inside the provider's range from `core/ids.hpp`.
2. A struct in `libpcomm/schema/pcomm.fbs` with the `// type N, kind` comment,
   and `kSchemaHash` in `rec/stream_header.hpp` updated (`schema_check.py`
   prints the new value).
3. A case in `libpcomm/tests/schema_conformance.cpp`.
4. A `Table<>` specialization + `TableRecords` entry in `pcomm-read/main.cpp`
   (the compiler demands both or neither).

`tools/schema_check.py` runs at build time and cross-checks 1-3; it also holds
`tools/read_pcs.py`'s enum-name tables equal to pcomm-read's.

## Adding a provider

`providers/<name>/` building one `.so`: implement `provider::IProvider`,
`PCOMM_EXPORT_PROVIDER`, claim a record-id range in `core/ids.hpp`, declare it
at startup via `pcomm_engine_declare_provider` (the engine refuses overlapping
ranges), and add a registry row in `pcomm/main.cpp`. Hook definitions follow
`providers/ibverbs/ib_hooks.hpp`; the manager contract is documented in
`pcomm/hook/CompositeHookManager.hpp`.
