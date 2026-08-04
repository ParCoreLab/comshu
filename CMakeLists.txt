cmake_minimum_required(VERSION 3.20)
project(pcomm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PCOMM_BUILD_TESTS      "Build core tests (no GPU required)" ON)
option(PCOMM_PROVIDER_GPUMEM  "Build the GPU memory-access provider (needs CUDA + NVBit)" OFF)
option(PCOMM_PROVIDER_IBVERBS "Build the ibverbs provider (needs libibverbs)" OFF)
# Separate from IBVERBS rather than implied by it: this one captures the DEVX
# control plane that IBGDA uses instead of verbs, and it needs libmlx5 as well.
option(PCOMM_PROVIDER_MLX5DV  "Build the mlx5dv/DEVX provider (needs libmlx5)" OFF)

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
add_subdirectory(pcomm)
add_subdirectory(pcomm-read)

if(PCOMM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
