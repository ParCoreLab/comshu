cmake_minimum_required(VERSION 3.20)
project(pcomm VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd; see .clangd

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PCOMM_BUILD_TESTS      "Build core tests (no GPU required)" ON)
option(PCOMM_PROVIDER_GPUMEM  "Build the GPU memory-access provider (needs CUDA + NVBit)" OFF)
option(PCOMM_PROVIDER_IBVERBS "Build the ibverbs provider (needs libibverbs)" OFF)
# Separate from IBVERBS rather than implied by it: this one captures the DEVX
# control plane that IBGDA uses instead of verbs, and it needs libmlx5 as well.
option(PCOMM_PROVIDER_MLX5DV  "Build the mlx5dv/DEVX provider (needs libmlx5)" OFF)
option(PCOMM_PROVIDER_UCX     "Build the UCX/UCP provider (needs libucp)" OFF)
# Headers only — this one links against no NCCL. It reaches libnccl through the
# PROFAPI (pnccl*) symbols the application has already loaded, which is why it
# is the one provider whose door is supported rather than interposed.
option(PCOMM_PROVIDER_NCCL    "Build the NCCL host-API provider (needs nccl.h)" OFF)

# Kernel lifetimes, via CUPTI. Not a provider — it captures no transport — and
# not part of the engine, which may not include a vendor header. Compiles no
# device code, so unlike gpumem it needs no nvcc and no separate build tree;
# its own CMakeLists skips the target (loudly) when CUPTI is not found.
option(PCOMM_CUPTI "Build the CUPTI kernel-timing backend (needs libcupti)" OFF)

# ── The C++ runtime: carried, not required ─────────────────────────────────
#
# Every object in this project is built with a toolchain the machine it runs on
# does not have. MN5's system libstdc++.so.6 tops out at GLIBCXX_3.4.29;
# gcc/13.2.0 builds ours against 3.4.30–3.4.32. So `./pcomm list` in a login
# shell has always died with
#   /lib64/libstdc++.so.6: version `GLIBCXX_3.4.30' not found
# and every case, script and check has had to `module load gcc/13.2.0` first.
# That is not a papercut: it is what made the provider pre-flight (`4e8d9bf`)
# report all 23 cases as build holes — it could not run the launcher, and the
# launcher's failure code is indistinguishable from "provider .so missing".
#
# Measured on MN5 before this was turned on:
#   pcomm, engine, ibverbs/mlx5dv/ucx/ucx_deep/nccl   GLIBCXX_3.4.30–3.4.32
#   gpumem (gcc-11.4 host), cupti                     GLIBCXX_3.4.29   (fine)
#   libduckdb.so (vendor prebuilt)                    GLIBCXX_3.4.22   (fine)
# So the dependency is entirely ours, and DuckDB — the thing that looked like
# the heavy dependency — was never the problem.
#
# -static-libstdc++ ONLY, never -static-libgcc. GCC's manual is explicit that a
# library which throws or catches across a shared-object boundary must share one
# unwinder, and providers are .so files whose exceptions the engine may catch.
# The system libgcc_s.so.1 offers GCC_7.0.0 against our GCC_4.2.0, so keeping it
# dynamic costs nothing.
#
# --exclude-libs,ALL keeps the static runtime PRIVATE, and this was measured
# rather than assumed, because the engine is LD_PRELOADed into other people's
# processes and a second libstdc++ implementation in the global scope is exactly
# the "load-bearing luck" engine_abi.h refuses to rely on. Exported symbols,
# dynamic build vs static: 1999 vs 1998, with ZERO symbols exported by the
# static build that the dynamic one did not already export. The interposition
# surface is unchanged.
#
# Skipped for compilers that do not take the switch — nvc++ ("Unknown switch:
# -static-libstdc++"), which is what the gpumem tree configures with. That tree
# needs nothing here: its provider is built by the NVBit-mandated gcc-11.4 host
# and asks for 3.4.29, which every host already has.
option(PCOMM_STATIC_CXXLIB "Carry libstdc++ in the binaries instead of requiring it" ON)
if(PCOMM_STATIC_CXXLIB AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  # Capability, not identity. Being a GNU/Clang driver means the flag is
  # ACCEPTED; it does not mean it can be LINKED. -static-libstdc++ needs
  # libstdc++.a, which distributions ship in a separate, compiler-versioned
  # package (Fedora's libstdc++-static, RHEL's likewise) that is not installed
  # by default. On a host whose default g++ was 16 while only gcc 15 had the
  # archive, every link died with
  #   /usr/bin/ld.bfd: cannot find -lstdc++
  #   have you installed the static version of the stdc++ library ?
  # and the pre-commit gate then refused every commit for a reason that had
  # nothing to do with the change being committed. Asking the linker costs one
  # try-compile and turns that into a configure-time sentence.
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-static-libstdc++" PCOMM_HAVE_STATIC_LIBSTDCXX)
endif()
if(PCOMM_STATIC_CXXLIB AND PCOMM_HAVE_STATIC_LIBSTDCXX)
  # LINK_LANGUAGE:CXX excludes CUDA-linked targets: the device link runs through
  # nvcc, which does not take these either.
  # Spelled -Wl, rather than CMake's LINKER: prefix on purpose: the guard above
  # already restricts this to GNU/Clang drivers, and LINKER: inside a generator
  # expression is one indirection too many to be obvious.
  add_link_options("$<$<LINK_LANGUAGE:CXX>:-static-libstdc++>"
                   "$<$<LINK_LANGUAGE:CXX>:-Wl,--exclude-libs,ALL>")
elseif(PCOMM_STATIC_CXXLIB AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  # Loud, not silent, and this is the whole reason the check is here rather than
  # a quiet fallback: carrying libstdc++ is load-bearing on MareNostrum 5, whose
  # system libstdc++.so.6 tops out at GLIBCXX_3.4.29 while gcc/13.2.0 builds
  # against 3.4.30-3.4.32. A build that quietly went dynamic there would link
  # clean and then die at startup with `GLIBCXX_3.4.30' not found — the exact
  # failure this option was added to prevent. Degrading is right on a
  # workstation; doing it without saying so is not.
  message(WARNING
    "pcomm: -static-libstdc++ does not link with this toolchain, so the "
    "binaries will REQUIRE ${CMAKE_CXX_COMPILER_ID}'s libstdc++ at run time. "
    "That is fine locally and NOT fine for a cluster install, where the system "
    "libstdc++ is usually older than the one this was built against. Install "
    "the static runtime (Fedora/RHEL: libstdc++-static, matching your default "
    "gcc) to get a portable binary, or pass -DPCOMM_STATIC_CXXLIB=OFF to say "
    "you meant it and silence this.")
elseif(PCOMM_STATIC_CXXLIB)
  message(STATUS "pcomm: PCOMM_STATIC_CXXLIB ignored — ${CMAKE_CXX_COMPILER_ID} "
                 "does not accept -static-libstdc++; this build will need its "
                 "toolchain's libstdc++ at run time")
endif()

# Parquet + SQL in pcomm-read, via DuckDB. OFF by default and that default is
# load-bearing, not caution: MN5 has no network and no cmake outside `module
# load` (verified 2026-08-09), and every suite case runs `pcomm-read --csv`
# there. A mandatory DuckDB would break the cluster path the whole test suite
# depends on. Parquet is produced where the UI runs, which is not the cluster.
# Fetch the library with tools/get_duckdb.sh, then configure with -DPCOMM_DUCKDB=ON.
option(PCOMM_DUCKDB "Build pcomm-read with DuckDB (--parquet, --sql)" OFF)

# Static DuckDB, from the vendor's own `static-libs-linux-amd64.zip`. The
# ergonomic difference is the whole point: with it, `pcomm` is ONE file that
# runs anywhere — measured, a probe linked this way needs only libc, libm and
# ld-linux, and answers a query under `env -i`. With the shared library the
# binary carries an RPATH into whichever directory .tools/duckdb happened to be,
# so copying or installing it breaks the derived layer in a way that reads as
# "this build has no DuckDB".
#
# Not the default: the archives are 130 MB unpacked (30 MB as the published
# zip), against 70 MB for the .so, and every relink of `pcomm` pays ~3 s to
# pull them in. That is a fine trade on the cluster, where the alternative is
# pushing a shared library and hoping it is found, and a poor one on a
# workstation that rebuilds the launcher fifty times a day.
option(PCOMM_DUCKDB_STATIC "Link DuckDB statically (tools/get_duckdb.sh --static)" OFF)

# The web UI, which is a DuckDB consumer but not the same decision.
#
# It used to BE the same decision — entering pcomm-ui whenever PCOMM_DUCKDB was
# on — and that coupling is why MN5 needs two untracked artifacts instead of
# one: pcomm-ui refuses to configure without a built `ui/dist`, so a cluster
# that has libduckdb but no frontend bundle cannot have Parquet or `pcomm sql`
# either. run_on_mn5.sh carries a paragraph about pushing both, and when either
# is missing the whole derived layer goes dark — which is how the suite's
# derived-layer assertions ended up unable to run on the very board they exist
# to protect (PLAN-20260904-1900, L1's first deviation).
#
# Defaults to following PCOMM_DUCKDB so existing invocations are unchanged.
option(PCOMM_UI "Build the `pcomm ui` server (needs DuckDB and a built ui/dist)"
       ${PCOMM_DUCKDB})

# ── Install layout ─────────────────────────────────────────────────────────
# Deliberately a literal lib/, not GNUInstallDirs' LIBDIR: on RHEL-likes that
# resolves to lib64/, and the launcher looks for providers in exactly
# ../lib/pcomm relative to itself (pcomm/main.cpp:find_so). One spelling, in
# both places, so an installed tree needs no PCOMM_PROVIDER_PATH.
#
#   <prefix>/bin/{pcomm,pcomm-read}
#   <prefix>/lib/pcomm/{libpcomm_engine,libibverbs_trace,libgpumem_trace}.so
#
set(PCOMM_INSTALL_LIBDIR "lib/pcomm" CACHE STRING
    "Where the engine and provider .so files are installed, under the prefix")

# Extra absolute directories to bake into installed binaries' RUNPATH. This is
# how an installed pcomm runs in a login shell with no `module load`: point it
# at the toolchain libstdc++ it was built against, e.g. on MareNostrum 5
#   -DPCOMM_INSTALL_RPATH_EXTRA=/apps/ACC/GCC/13.2.0/lib64
# Semicolon-separated. New dtags are used throughout, so RUNPATH is searched
# *after* LD_LIBRARY_PATH and a user who module-loads another toolchain wins.
set(PCOMM_INSTALL_RPATH_EXTRA "" CACHE STRING
    "Extra absolute RUNPATH entries for installed binaries (semicolon-separated)")

# Whether this build tree installs the core (engine, launcher, reader, headers)
# or only its provider.
#
# It defaults OFF for a gpumem-only tree, and that default is a safety belt with
# a scar behind it. gpumem must be configured in its own tree against the NVHPC
# toolchain (tests/hpc/run_on_mn5.sh), where CMAKE_CXX_COMPILER is nvc++ — and
# nvc++ compiles libpcomm_engine.so into something that does not work: it emits
# an *undefined reference* to __builtin_ia32_rdtsc (a GCC/Clang builtin) instead
# of inlining it, so every hooked call dies with
#   symbol lookup error: libpcomm_engine.so: undefined symbol: __builtin_ia32_rdtsc
# The build tree never exposed this because run_on_mn5.sh copies only
# libgpumem_trace.so out of build-gpumem, and its $ORIGIN/../../libpcomm rpath
# then finds the *gcc-built* engine. `cmake --install build-gpumem` had no such
# luck: it dropped the nvc++ engine over the good one and broke every capture.
#
# The root cause (the engine's rdtsc should be written so any compiler can build
# it) is worth fixing on its own; until then, a gpumem tree installs its
# provider and nothing else.
if(PCOMM_PROVIDER_GPUMEM AND NOT PCOMM_PROVIDER_IBVERBS)
  set(_pcomm_install_core_default OFF)
else()
  set(_pcomm_install_core_default ON)
endif()
option(PCOMM_INSTALL_CORE
       "Install the engine, launcher, reader and headers from this build tree"
       ${_pcomm_install_core_default})

# enable_testing() must come before any add_subdirectory that calls add_test:
# CTest silently drops tests registered in a directory that was added first.
# libpcomm/ registers the schema tests, and while enable_testing() sat at the
# bottom of this file they were never in the ctest list at all — `ctest -R
# schema` reported "No tests were found", which is indistinguishable from
# passing if you only look at the exit code of a full `ctest` run.
if(PCOMM_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(libpcomm)
add_subdirectory(providers)
if(PCOMM_CUPTI)
  add_subdirectory(gpu-backends/cupti)
endif()
# The halves of the one `pcomm` binary, before the binary that links them:
# pcomm-read and pcomm-ui are OBJECT libraries, and pcomm/ names both targets.
add_subdirectory(pcomm-read)
# The UI is a DuckDB consumer by definition — serving a capture is querying it —
# so it cannot exist without one. It is now its own option (see PCOMM_UI above):
# on MN5 this subdirectory is never entered, DuckDB or not, and `pcomm ui`
# reports that this build has no such verb.
if(PCOMM_UI AND NOT PCOMM_DUCKDB)
  message(FATAL_ERROR "PCOMM_UI=ON needs PCOMM_DUCKDB=ON — serving a capture is querying it")
endif()
if(PCOMM_UI)
  add_subdirectory(pcomm-ui)
endif()
add_subdirectory(pcomm)

if(PCOMM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
