# pcomm — ParCore Lab Communication Profiler

Unified GPU-communication profiler merging **SigmaTrace**, **Snoopie**, snoopie-sanitized,
and (later) ucTrace. Design docs live one level up: [`../UNIFICATION.md`](../UNIFICATION.md)
(what/why) and [`../ARCHITECTURE.md`](../ARCHITECTURE.md) (layers, interfaces, patterns).

| Piece | Name |
|---|---|
| Core library (L0–L2) | `libpcomm` |
| The command (launch, read, export, query, serve) | `pcomm` |
| Namespace | `pcomm::` (`core`, `rec`, `hook`, `attr`, `gpu`, `provider`) |
| Env prefix | `PCOMM_*` |

[`BUGS-FOUND.md`](BUGS-FOUND.md) — defects in other software that pcomm found,
with the evidence that identified each and the explanations that were excluded.
The claim a tool makes is what it found, not what it can in principle see.

## Layout

```
libpcomm/        L0–L2 core — vendor-free (no cuda*/hip*/ibv_*/Sanitizer_* types allowed)
                 checked by tools/check_vendor_free.py (ctest `vendor_free`)
  include/pcomm/
    core/        record model (Event/Entity/Artifact/Edge), time, ids, neutral vocabulary
    rec/         Recorder, ThreadLocalBuffer, InternTable, sinks (the hot path)
    hook/        HookTrait / CompositeHookManager        [ported from SigmaTrace/Hook]
    attr/        Collector/Filter/Attributor pipeline    [from snoopie-sanitized design]
providers/       L3 — each builds one .so (vendor types live HERE, not in core)
  gpumem/        GPU memory-access provider (Snoopie engine)
  ibverbs/       proof-of-concept network provider (from SigmaTrace)
  ucx/           UCX as the application asked for it (public UCP symbols)
  ucx_deep/      UCX as it reached the wire (the uct_iface_ops_t table)
gpu-backends/    vendor capture engines feeding the gpumem provider
  nvbit/         phase-1 default (confirmed working on du04: CUDA 13.0, driver 580)
  sanitizer/     Compute-Sanitizer (stub; phase 2)
pcomm/           the binary: verb dispatch (main.cpp) + the launcher (run.cpp)
pcomm-read/      the reading half — read/dump/export/link/sql/types, linked into pcomm
pcomm-ui/        the serving half — `pcomm ui`, linked in when built with DuckDB
ui/              the Svelte frontend, built to ui/dist and embedded in the binary
tests/           core unit/smoke tests (no GPU required)
```

The vendor-free rule bans **types and headers**, not names. Core may *name* a
vendor symbol, because interposition and `dlsym` work by name and need no
header: `core/mem_attr.hpp` declares libcuda's signature itself rather than
including `cuda.h`, `core/dmabuf_registry.hpp` copies `CUresult`'s ABI under the
name `CuResult`, and `src/engine.cpp` defines five `extern "C"` doors whose
signatures are `void*` and `size_t` throughout. The property being protected is
that core compiles and runs on a host with no CUDA and no verbs — which is
MN5's own core partition. `tools/check_vendor_free.py` enforces exactly that,
and carries the allowlist of symbols core interposes on purpose.

## Usage

Pick what to capture by short name; `pcomm` assembles the run and reads it back.

```bash
pcomm                                  # the verbs
pcomm list                             # what can I capture, and how?
pcomm run -p ib      -- ./app args     # InfiniBand wire traffic (inter-node)
pcomm run -p p2p     -- ./app args     # GPU-initiated NVLink peer r/w (intra-node, Snoopie)
pcomm run -p ib,p2p  -- ./app args     # both — see "replay" below
pcomm read pcomm-out                   # what is in the capture
pcomm export pcomm-out --csv out.csv   # tables for anything that reads CSV
pcomm ui pcomm-out                     # the panels, at 127.0.0.1:8777
agent/run.sh pcomm-out                 # ... and the chat pane, if a model is up
```

`agent/` is an optional sidecar that lets you ask the capture questions in
English and get prose, the SQL behind it, and a chart. It answers with a query
rather than with data, so every number it gives you is one click from being
re-run in the Ask panel — see [`agent/README.md`](agent/README.md). The panels
work exactly the same without it.

One binary with verbs, not three tools: `pcomm-read` and `pcomm-ui` are gone as
executables (they are still the directories the code lives in). It also removes
an argv[1] that used to mean a *capture* in some invocations and an *export* in
others — `read`/`dump`/`export` take a capture, `link`/`sql` take an export.

Providers are chosen with `-p name[,name...]` (each accepts short aliases, e.g.
`ib`/`net`, `p2p`/`nvlink`/`snoopie`). Add `--dry-run` to print the plan.

**Replay.** Some engines must own the whole runtime — NVBit (`p2p`) takes over
the CUDA runtime and `dlsym`, so it can't share a process with anything else.
When a selection mixes such an *exclusive* provider with others, one process
can't host them, so `pcomm` **replays**: it runs the workload once per exclusive
provider and drops each pass under one `-o` directory (`out/ib/`, `out/gpumem/`,
…). The passes are wall-clock-aligned, and `pcomm read` stitches them back into
one per-rank picture (each provider owns a disjoint record-type range, so a
rank's network and NVLink traffic merge cleanly). Hook-based providers (all the
network ones) share a single pass — no replay needed among them.

## Phase 1 scope (decided 2026-07-15)

1. New four-archetype record model with interning, from day one.
2. gpumem provider with the **NVBit** backend first (Sanitizer stubbed).
3. **ibverbs** as the single network proof provider; the rest of SigmaTrace's
   providers follow in phase 2. ucTrace deferred.
   *(Superseded 2026-08-27: `ucx` and then `ucx_deep` landed in phase 1. The
   ids 800-899 reserved for "ucTrace" are now the UCT-layer provider, which
   reaches UCX's data plane through the `uct_iface_ops_t` dispatch table rather
   than by reimplementing UCT internals — see
   `plans/PLAN-20260827-1449-ucx-deep-uct-ops-table-in-progress.md`.)*
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

## Install

`cmake --install` lays down a self-locating tree — the launcher finds the
engine through `$ORIGIN/../lib/pcomm` and its providers through the matching
search root, so an installed `pcomm` needs **no** `PCOMM_PROVIDER_PATH`,
`PCOMM_ENGINE_PATH` or `LD_LIBRARY_PATH`:

```
<prefix>/bin/pcomm
<prefix>/lib/pcomm/{libpcomm_engine,libibverbs_trace,libgpumem_trace}.so
<prefix>/include/pcomm/...
```

`PCOMM_INSTALL_RPATH_EXTRA` bakes absolute directories into the installed
RUNPATH, which is what lets the result run in a bare login shell with no
`module load`. On MareNostrum 5 (installed at `~/.local`, already on PATH):

```bash
# core + ibverbs, from the gcc-13.2 tree
cmake -B build -DPCOMM_PROVIDER_IBVERBS=ON \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local \
      -DPCOMM_INSTALL_RPATH_EXTRA=/apps/ACC/GCC/13.2.0/lib64
cmake --build build -j && cmake --install build

# the CUDA provider, from its own NVHPC tree (see run_on_mn5.sh for the flags)
cmake --install build-gpumem              # installs gpumem only, by design
```

That second command installs *only* the provider: `PCOMM_INSTALL_CORE` defaults
OFF for a gpumem-only tree, because nvc++ miscompiles the engine (see
`things-to-circle-back-to.md`). Don't override it.

New dtags are used throughout, so the baked RUNPATH is searched *after*
`LD_LIBRARY_PATH` — module-loading a different toolchain still wins. The one
runtime dependency an install cannot supply is `nvdisasm` on `PATH`, which NVBit
needs when `-p p2p` actually runs on a GPU node.

## Formatting

`.clang-format` is the house style, derived from the tree rather than picked:
4-space indent, `char* p`, unindented namespaces, 100 columns. Two settings are
load-bearing and shouldn't be relaxed without reading the comments on them —
`ReflowComments: false` (the comments here carry tables and aligned prose that
reflowing destroys) and `SortIncludes: false` (includes are grouped on purpose,
several with explanatory trailing comments).

```bash
tools/format.sh              # format the staged files
tools/format.sh --all        # format every tracked first-party source
tools/format.sh --check --all
```

`third_party/`, the vendored NVBit, and `libpcomm/schema/generated/` each carry
a `DisableFormat: true` marker, so neither the hook nor an editor-on-save will
touch them.

The pre-commit hook enforces formatting **only once the whole tree is already
formatted** — until then it stays inert, so a one-file commit never drags a
repo-wide reformat behind it. Run `tools/format.sh --all` once, commit that on
its own, and enforcement switches on by itself.

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
  gate; a change is not done until this board is green. It syncs by pushing
  `HEAD` to a bare repo on MN5 which the remote build tree clones, so every
  board is attributable to a commit; a dirty tree is refused (`--rsync` to
  override while iterating).

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
`tools/read_pcs.py`'s enum-name tables equal to the reader's.

## Adding a provider

`providers/<name>/` building one `.so`: implement `provider::IProvider`,
`PCOMM_EXPORT_PROVIDER`, claim a record-id range in `core/ids.hpp`, declare it
at startup via `pcomm_engine_declare_provider` (the engine refuses overlapping
ranges), and add a registry row in `pcomm/main.cpp`. Hook definitions follow
`providers/ibverbs/ib_hooks.hpp`; the manager contract is documented in
`pcomm/hook/CompositeHookManager.hpp`.
