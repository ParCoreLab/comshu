# common.sh — shared environment for the pcomm integration suite on MareNostrum 5.
# Sourced by every case's job.sbatch. Override any of these from the environment.
#
# MN5 accelerated partition (Sapphire Rapids + 4× H100 + NDR InfiniBand):
#   account etur02, qos acc_debug (2 h / debug). See `bsc_queues` for the rest.

: "${PCOMM_ACCOUNT:=etur02}"
: "${PCOMM_QOS:=acc_debug}"
: "${PCOMM_ROOT:=$HOME/proj/pcomm}"
: "${PCOMM_BUILD:=$PCOMM_ROOT/build}"

# A JOB SUBMITTED FROM A GPP LOGIN NODE INHERITS THE GPP MODULE TREE, and runs
# on an ACC node regardless. sbatch defaults to --export=ALL, so MODULEPATH
# arrives on the compute node still pointing at /apps/GPP/modulefiles — where
# `gromacs/2025.2-gcc-nvhpcx`, `fftw/3.3.10-gcc-nvhpcx` and the rest of the
# accelerated stack do not exist. Lmod then says only "module(s) are unknown",
# the module load is not fatal, and the failure surfaces further down as
# `mpirun was unable to find the specified executable: gmx_mpi` — which reads
# as a broken MPI launch rather than as a job looking in the wrong tree.
#
# Cost the first GROMACS round-2 submission (job 45235223, FAILED in 15 s). It
# only started mattering on 2026-08-30, when both ACC login nodes went down and
# submissions moved to glogin1; the suite's own cases never noticed because
# they resolve the SDK by globbing /apps/ACC/... directly rather than by module.
#
# Scoped to inside a job on purpose. On a login node MODULEPATH is correct for
# the hardware you are on, and swapping it there would change which compilers
# run_on_mn5.sh builds with — a working path this has no business touching.
if [ -n "${SLURM_JOB_ID:-}" ] && [ -d /apps/ACC/modulefiles ]; then
    case "${MODULEPATH:-}" in
        */apps/GPP/modulefiles*)
            export MODULEPATH="${MODULEPATH//\/apps\/GPP\/modulefiles//apps/ACC/modulefiles}"
            # AND PURGE, because the path was only half of it. glogin's login
            # profile auto-loads impi/2021.10.0, mkl, UCX/1.15.0 and bsc, and
            # those ride into the job too. The ACC GROMACS modulefile declares
            # `conflict("impi")`, so with the tree corrected it still refused:
            #   Cannot load module "gromacs/2025.2-gcc-nvhpcx" because these
            #   module(s) are loaded: impi
            # An inherited GPP module is never something an ACC job wanted, so
            # dropping all of them is the honest state rather than unloading the
            # one name that happened to collide today.
            #
            # Safe before pcomm_load_runtime, which every job calls next and
            # which loads gcc/13.2.0 and nvidia-hpc-sdk/25.7 from the corrected
            # tree — and those are exactly the prereqs the application modules
            # declare (`prereq("nvidia-hpc-sdk")` in gromacs, `prereq_any` in
            # fftw). Verified end to end on glogin1: purge, gcc, hpc-sdk, fftw,
            # gromacs leaves gmx_mpi at /apps/ACC/GROMACS/2025.2/GCC/NVHPCX/bin.
            module purge >/dev/null 2>&1 || true
            echo "common.sh: GPP module environment inherited by an ACC job — MODULEPATH swapped to /apps/ACC/modulefiles and modules purged" >&2
            ;;
    esac
fi

# Tools + providers (the launcher finds .so via PCOMM_PROVIDER_PATH, colon-list).
# One binary, verbs for everything: `$PCOMM run`, `$PCOMM read`, `$PCOMM export`.
# There is no PCOMM_READ any more — there is no second executable to point it at.
export PCOMM="$PCOMM_BUILD/pcomm/pcomm"
# Every provider directory, and an existing value is PREPENDED rather than
# replaced: a caller that already pointed at a specific build (run_on_a100.sh
# does) must not have it silently overwritten by sourcing this file. That
# overwrite was live — the runner exported the mlx5dv path, common.sh replaced
# it with ibverbs:gpumem, and `-p mlx5dv` would have failed to find its
# provider on a host where the provider had just been built successfully.
#
# GLOBBED, not listed, since 2026-08-27. The hand-written list named five
# providers and `ucx_deep` was the sixth: its library built correctly on MN5 and
# the launcher could not find it, so `pcomm run -p ucx,ucx_deep` refused the
# whole run (job 45112957, "ucx_deep provider library not found"). Refusing was
# right — a capture missing a provider the caller asked for is worse than no
# capture — but the list was the defect, and a list of provider directories kept
# in step with the provider directories by hand is a list kept in step by
# nobody. libpcomm/CMakeLists.txt makes the same argument about globbing the
# record headers, and cites a build it already cost (bea74d8).
#
# Only directories that actually hold a provider library. `providers/*/` also
# matches CMakeFiles and any other build artefact, and while a junk entry is
# harmless — the launcher searches the path and reports what it cannot find —
# putting build scaffolding in a search path is how a search path stops being
# readable. The name is the one the launcher looks for.
#
# gpu-backends/ IS SEARCHED TOO, and it has to be. `pcomm list` and
# `pcomm run -p` do not distinguish a provider from a backend — `cupti` is a
# registry row in pcomm/run.cpp exactly as `ibverbs` is, and the launcher looks
# for its library on this one path — but the source tree does: libcupti_trace.so
# builds under gpu-backends/, not providers/, because it captures no transport.
# Globbing only providers/ therefore reproduced the ucx_deep failure recorded
# above, for a directory rather than for a name: the library builds correctly and
# `pcomm run -p cupti` refuses the run because it cannot find it.
_pcomm_provider_path=""
for _pdir in "$PCOMM_BUILD"/providers/*/ "$PCOMM_BUILD"/gpu-backends/*/; do
  [ -d "$_pdir" ] || continue            # unmatched glob, or no providers built
  # NOT `set --`: this file is SOURCED, and rewriting the caller's positional
  # parameters to test a glob would break the first job script that uses "$1".
  _pcomm_has_so=0
  for _so in "$_pdir"lib*_trace.so; do [ -e "$_so" ] && _pcomm_has_so=1; done
  [ "$_pcomm_has_so" -eq 1 ] || continue  # built nothing, or not a provider dir
  _pcomm_provider_path="${_pcomm_provider_path:+$_pcomm_provider_path:}${_pdir%/}"
done
export PCOMM_PROVIDER_PATH="${PCOMM_PROVIDER_PATH:+$PCOMM_PROVIDER_PATH:}$_pcomm_provider_path"
unset _pdir _so _pcomm_has_so _pcomm_provider_path
# Providers link the shared engine (libpcomm_engine.so); they carry an $ORIGIN
# rpath to it, but export the dir too so a provider .so copied elsewhere (as the
# gpumem build does) still resolves it.
export LD_LIBRARY_PATH="$PCOMM_BUILD/libpcomm:${LD_LIBRARY_PATH:-}"

# NCCL's profiler plugin door — the SAME library as the interposer.
#
# NCCL passes this value straight to dlopen when it contains a '/'
# (reference/nccl-git/src/plugin/plugin_open.cc:79-95), so pointing it at the
# already-LD_PRELOADed libnccl_trace.so gets the resident handle back: one
# process, one recorder, and the plugin callbacks share the interposer's state.
# Without this the 900-910 tables are complete and the 912-916 ones are empty —
# the capture still says what the application ASKED for, and says nothing about
# what NCCL CHOSE.
#
# Set only when the library actually exists, and never over a caller's value. A
# path to a missing file is not a loud failure: NCCL logs "Could not find" at
# INFO level, which nothing reads, and runs on.
if [ -z "${NCCL_PROFILER_PLUGIN:-}" ]; then
  for _so in "$PCOMM_BUILD"/providers/nccl/libnccl_trace.so; do
    [ -e "$_so" ] && export NCCL_PROFILER_PLUGIN="$_so"
  done
  unset _so
fi

# NVIDIA HPC SDK. MN5's path was hardcoded here, and the SDK version and CUDA
# version were baked into the NCCL/NVSHMEM paths with it — which silently broke
# the moment a case ran anywhere else. On du04 the SDK is 25.9 under /opt with
# CUDA 13.0, so every one of those literals was wrong and NVSHMEM_HOME pointed
# at a directory that does not exist. Resolved by search now, with the env
# override kept first so a host with an unusual layout can still say so.
if [ -z "${PCOMM_SDK:-}" ]; then
  for _c in /apps/ACC/NVIDIA-HPC-SDK/*/Linux_x86_64/* /opt/nvidia/hpc_sdk/Linux_x86_64/*; do
    case "$_c" in *\**) continue ;; esac          # unmatched glob
    [ -d "$_c/comm_libs" ] && PCOMM_SDK="$_c"      # last match wins = newest
  done
fi
export PCOMM_SDK="${PCOMM_SDK:-}"

# comm_libs is versioned by CUDA (12.9 on MN5, 13.0 on du04) and NCCL by its own
# release, so both are globbed rather than spelled. Env still wins.
_pcomm_first_dir() { for _d in "$@"; do [ -d "$_d" ] && { echo "$_d"; return; }; done; }
export NVSHMEM_HOME="${NVSHMEM_HOME:-$(_pcomm_first_dir \
  "$PCOMM_SDK"/comm_libs/*/nvshmem "$PCOMM_SDK"/comm_libs/nvshmem)}"
export NCCL_HOME="${NCCL_HOME:-$(_pcomm_first_dir \
  "$PCOMM_SDK"/comm_libs/*/nccl-* "$PCOMM_SDK"/comm_libs/*/nccl "$PCOMM_SDK"/comm_libs/nccl)}"

# Runtime toolchain: the providers need gcc-13.2 libstdc++ (GLIBCXX_3.4.31);
# compile CUDA with gcc-11.4 host (nvcc 12.9 rejects gcc-13.2's __is_convertible).
#
# `module` is a MareNostrum thing. On a host without it these are no-ops, so the
# toolchain has to already be on PATH — which is why nvcc is located explicitly
# below rather than assumed. Getting this wrong cost a green board that had
# built nothing: nvcc was missing, the case skipped, and the skip read as a pass.
_pcomm_module() { command -v module >/dev/null 2>&1 && module "$@" >/dev/null 2>&1; return 0; }
pcomm_load_runtime() { _pcomm_module load gcc/13.2.0 nvidia-hpc-sdk/25.7; pcomm_find_cuda; }
pcomm_load_build()   { _pcomm_module load gcc/11.4.0 nvidia-hpc-sdk/25.7; pcomm_find_cuda; }

# Put nvcc on PATH if it is not already. Returns non-zero when there is no CUDA
# at all, so a caller can tell "no CUDA here" from "CUDA is fine".
pcomm_find_cuda() {
  command -v nvcc >/dev/null 2>&1 && return 0
  for _b in "$PCOMM_SDK"/compilers/bin "$PCOMM_SDK"/cuda/bin /usr/local/cuda/bin; do
    if [ -x "$_b/nvcc" ]; then export PATH="$_b:$PATH"; return 0; fi
  done
  return 1
}

# Where a case drops its capture + where the checker reads it.
pcomm_out() { echo "${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/$1-${SLURM_JOB_ID:-local}"; }

# The UCX tree, anchored to the MPI THAT WILL ACTUALLY RUN — not to $PCOMM_SDK.
#
# $PCOMM_SDK is the NEWEST SDK (26.5 on MN5) while pcomm_load_runtime loads
# nvidia-hpc-sdk/25.7, so mpirun comes from 25.7 and the UCX it dlopens is 25.7's.
# Deriving UCX from $PCOMM_SDK instead picked 26.5, whose hpcx layout is a
# different version entirely (2.50 vs 2.22.1) — case 19 refused to run against it
# and said so, which is the only reason this is a two-line fix rather than a
# capture full of nothing. Job 44696379.
#
# The provider is INTERPOSED on public UCP symbols, so "which libucp" is not a
# detail: build against one and run against another and the interposition simply
# does not engage. Build and run therefore share this one definition — run_on_mn5.sh
# calls it too rather than spelling a path of its own.
#
# MN5 has several hpcx trees per SDK (2.14, 2.20, 2.22.1); the newest that
# actually carries lib/libucp.so wins, which is the one HPC-X's own `latest`
# symlink points at.
pcomm_find_ucx() {
  [ -n "${UCX_HOME:-}" ] && { echo "$UCX_HOME"; return 0; }
  _mpirun="$(command -v mpirun 2>/dev/null)" || return 1
  [ -n "$_mpirun" ] || return 1
  # $SDKROOT/comm_libs/mpi/bin/mpirun -> $SDKROOT
  _sdk="$(cd "$(dirname "$_mpirun")/../../.." 2>/dev/null && pwd)" || return 1
  _best=""
  for _u in "$_sdk"/comm_libs/*/hpcx/hpcx-*/ucx; do
    [ -e "$_u/lib/libucp.so" ] && _best="$_u"      # last match wins = newest
  done
  [ -n "$_best" ] || return 1
  echo "$_best"
}

# The linked Parquet export, beside the CSV one, when this build can make it.
#
# WHY BOTH AND NOT JUST ONE. CSV is the path that always works: MN5 defaults to
# PCOMM_DUCKDB=OFF because tools/get_duckdb.sh needs network and MN5 has none, so
# a suite that required Parquet would not run on the cluster it exists for.
# Parquet is the only path that carries the DERIVED layer — comm_edges as
# link_sql.hpp actually computes it, plus object_access, link_health,
# rank_identity, endpoint, timeline_span, wr_flow — none of which the CSV export
# emits and therefore none of which any assertion could reach.
#
# SILENT WHEN UNAVAILABLE, on purpose. This must never turn a working suite red
# on a host without DuckDB; sql_check.sh notices the export is missing and runs
# exactly as it did before. The one thing it must not do is fail *quietly when it
# was supposed to work*, so a build that HAS the verb and still errors says so.
pcomm_derive() {
  _out="$1"
  # Attempting it IS the capability test. A build without DuckDB still advertises
  # `sql` and `--parquet` in its verb list and refuses at runtime, so asking the
  # verb list would answer yes and then fail.
  if ! "$PCOMM" export "$_out" --parquet "$_out.pq" >/dev/null 2>&1; then
    echo "   (no parquet export from this build — derived-layer assertions will be skipped)"
    rm -rf "$_out.pq"
    return 0
  fi
  "$PCOMM" link "$_out.pq" >/dev/null 2>&1 \
    || { echo "!! parquet export succeeded but \`pcomm link\` failed on $_out.pq" >&2
         rm -rf "$_out.pq"; return 0; }
  echo "   (derived tables in $_out.pq)"
}
