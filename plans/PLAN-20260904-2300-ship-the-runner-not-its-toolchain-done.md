# PLAN — ship the runner, not its toolchain

- **Created:** 2026-09-04 23:00 +03
- **Status:** done for the launcher and the core providers (`f97c728`, MN5 board
  45427910 green). Two gaps left, both named below.
- **Related:** `4e8d9bf` (the pre-flight that could not run the launcher),
  `PLAN-20260904-1900` (its `pcomm --version` residual), `tools/get_duckdb.sh`,
  the `PCOMM_INSTALL_RPATH_EXTRA` hook in the root CMakeLists.

## The problem, in one line

**Every binary this project builds requires a toolchain the machine it runs on
does not have, and every script has had to know that.**

    $ ./build/pcomm/pcomm list
    /lib64/libstdc++.so.6: version `GLIBCXX_3.4.30' not found

That is not a papercut. It is what made the provider pre-flight report all 23
cases as build holes on a build that had just compiled every one of them: the
check could not run the launcher, and the launcher's failure code (1) is
indistinguishable from "this provider's .so is missing". Every case, every
probe and every hand-run has had to `module load gcc/13.2.0` first, and the
failure when you forget reads as a broken cluster rather than a missing module.

## What the measurement changed

The starting hypothesis was that DuckDB — 70 MB of shared library, pushed by
rsync, gated behind `PCOMM_DUCKDB` — was the heavy dependency to attack. It is
not the problem at all:

| object | needs |
|---|---|
| `pcomm`, engine, ibverbs/mlx5dv/ucx/ucx_deep/nccl | GLIBCXX_3.4.30–3.4.32 |
| gpumem (gcc-11.4 NVBit host), cupti | GLIBCXX_3.4.29 |
| `libduckdb.so` (vendor prebuilt) | GLIBCXX_3.4.22 |
| **MN5's system `libstdc++.so.6`** | **GLIBCXX_3.4.29** |

The vendor's library is built on an old toolchain and would have run there all
along. The dependency is entirely ours, and it is ours only because gcc/13.2.0
is what we compile with. DuckDB's *shape* is still worth changing (below), but
for a different reason than the one we started with.

## What landed

**1. `PCOMM_STATIC_CXXLIB` (default ON)** — `-static-libstdc++`, applied to
CXX-linked targets only. Verified on MN5:

    env -i PATH=/usr/bin:/bin build-static/pcomm/pcomm run -p ib -o /tmp/x -- /bin/true

produces a capture with no modules loaded at all, where the same command
against the old build dies in `ld.so`.

Three details that are the whole safety argument:

- **`-static-libgcc` is deliberately NOT used.** GCC's manual is explicit that
  objects throwing or catching across a shared-object boundary must share one
  unwinder, and providers are `.so` files whose exceptions the engine may catch.
  The system `libgcc_s.so.1` offers `GCC_7.0.0` against our `GCC_4.2.0`.
- **`--exclude-libs,ALL`, and it was measured.** The engine is LD_PRELOADed into
  other people's processes, so a second libstdc++ *implementation* in the global
  symbol scope is exactly the "load-bearing luck" `engine_abi.h` refuses to rely
  on. Exported symbols, dynamic vs static build of the engine: **1999 vs 1998,
  with zero symbols exported by the static build that the dynamic one did not
  already export.** The interposition surface is unchanged; only the code behind
  the same weak template instantiations is.
- **nvc++ rejects the switch** ("Unknown switch: -static-libstdc++"), and that is
  what the gpumem tree configures with. `$<LINK_LANGUAGE:CXX>` keeps it off
  nvcc's device link, and the guard on `CMAKE_CXX_COMPILER_ID` keeps it off the
  whole tree. Nothing is lost: gpumem and cupti ask for 3.4.29, which every host
  already has.

**2. `PCOMM_DUCKDB_STATIC` (default OFF, ON for the cluster)** — the vendor
publishes `static-libs-linux-amd64.zip` (30 MB, 22 archives, 130 MB unpacked).
A probe linked against it needs only libc, libm and ld-linux and answers a query
under `env -i`; the link costs **3.0 s**. The reason to prefer it is not size but
that the shared library is found through an RPATH into whichever source tree
built it — so the launcher was only runnable where it was built.

**3. `PCOMM_UI` is its own option.** It used to be a consequence of
`PCOMM_DUCKDB`, and `pcomm-ui` refuses to configure without a built `ui/dist` —
so a cluster with the library and no frontend bundle got no Parquet and no
`pcomm sql` either. The derived layer went dark for want of a web page nobody
was going to open on a login node, and `run_on_mn5.sh` had to push two untracked
artifacts and check for both. One now.

**4. `pcomm --version`**, printing the build id word for word as
`Invocation.build` carries it:

    commit=f97c728a4db3 type=unset abi=3 schema=0x024C07869755921D

An explicit residual of PLAN-20260904-1900: until now the only way to ask a
binary which commit it was, was to run a workload and read the capture.

### The bundle, end to end

`cmake --install build --prefix /tmp/pcomm-bundle` on MN5, then, in `env -i`
with no `PCOMM_PROVIDER_PATH` and no modules:

    $ /tmp/pcomm-bundle/bin/pcomm --version
    commit=f97c728a4db3 type=unset abi=3 schema=0x024C07869755921D
    $ /tmp/pcomm-bundle/bin/pcomm run -p ib -o /tmp/probe -- /bin/true
    pcomm: 1 provider → 1 pass

`objdump -p bin/pcomm` → `libm`, `libgcc_s`, `libc`, `ld-linux`. That is the
ergonomic claim in one line: **a directory you can copy to a cluster.**

Board 45427910: SUITE GREEN, 19 PASS, across NCCL, UCX/MPI, NVSHMEM, CUDA and
IB cases — i.e. the static runtime survives being preloaded into every C++
application the suite has.

## Round two: the providers go inside (`f023eb8`)

**The bundle was missing four of seven providers**, and no install rule can fix
that: `cmake --install` carries the targets of the tree it was given, and three
of this project's four trees exist precisely because they cannot be one tree.
The full set exists only as the copies `run_on_mn5.sh` makes into
`build/providers/`.

So the launcher carries them. `pcomm bake -o <new> <binary> <libs...>` appends
the libraries past the end of the ELF image, with a table and a 40-byte footer;
`pcomm bake --list` says what a binary carries; `pcomm list` says it too.

- **Appended rather than an ELF section.** `objcopy --add-section` is tidier and
  `readelf` can list it, but a non-allocated section is exactly what `strip`
  deletes — a payload that vanishes when someone strips a binary is worse than
  one that is obviously there.
- **Unpacked into a cache named by the archive's CONTENT hash.** Two builds can
  never share a cached copy. Given four false findings from stale binaries, a
  cache keyed on anything weaker would have been the fifth.
- **Write-to-temp + rename**, because sixteen ranks on a node unpack at once and
  a half-written library must never be dlopen'd.
- **Baked copies are the LAST resort** — `PCOMM_PROVIDER_PATH`, the binary's own
  directory, `../lib/pcomm`, then the payload. A baked launcher inside a build
  tree behaves exactly as it did before, which is what makes it safe for the
  cluster runner to bake in place at the end of every build.
- **The engine is baked too, and needs it most**: without it a run continues with
  a warning and captures far less (dlsym is the engine's), so a single-file
  launcher that could not find its engine would produce a quiet wrong answer.

Verified on MN5 with the real payload, with `build/libpcomm`, `build/providers`
and `build/gpu-backends` all renamed away so only the binary could answer:

    $ /tmp/onefile-real/pcomm --version
    commit=f023eb872525 type=RelWithDebInfo abi=3 schema=0x024C07869755921D
    $ env -i PATH=/usr/bin:/bin /tmp/onefile-real/pcomm run -p ib,cupti -o … -- /bin/true
    # unpacked: libpcomm_engine.so, libibverbs_trace.so, libcupti_trace.so

127 MB for the one file: a 91 MB launcher (static DuckDB) plus 36 MB of
`--strip-debug`'d providers. Stripping debug info leaves both symbol tables
alone, so interposition and NVBit are untouched.

**`CMAKE_BUILD_TYPE=RelWithDebInfo` on both cluster runners.** Board 45441332 is
green with it, and every capture now reports `type=RelWithDebInfo` instead of
`type=unset`. Do NOT read the per-case timings as a measurement of the change:
total job time moved 14:57 → 13:28, but individual cases moved both ways (01:
113→40 s, 21: 118→62 s, but 15: 38→209 s and 19: 10→25 s), which is node and
network variance dominating. A real overhead comparison needs a controlled
experiment, not two board runs.

## What is left

- **`run_on_a100.sh` builds with the defaults**, so it gets the static runtime
  and RelWithDebInfo but not static DuckDB (no `.tools/duckdb-static` there —
  the a100 path rsyncs the tree without `.tools`) and does not bake.
- **Nothing rebakes on `--no-build`.** The baked payload is refreshed only by a
  build step, so a launcher baked yesterday and run today carries yesterday's
  providers — invisible unless the disk copies are gone. The provenance layers
  see it (the unpacked images carry their own mtimes), but only once it runs.
- **The overhead baseline moved.** Numbers recorded against `-O0` boards (the
  UCT `+39 ns`, the intern-cache work) are no longer comparable to what this
  board produces.
