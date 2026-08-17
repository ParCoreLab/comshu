# common.sh — shared environment for the pcomm integration suite on MareNostrum 5.
# Sourced by every case's job.sbatch. Override any of these from the environment.
#
# MN5 accelerated partition (Sapphire Rapids + 4× H100 + NDR InfiniBand):
#   account etur02, qos acc_debug (2 h / debug). See `bsc_queues` for the rest.

: "${PCOMM_ACCOUNT:=etur02}"
: "${PCOMM_QOS:=acc_debug}"
: "${PCOMM_ROOT:=$HOME/proj/pcomm}"
: "${PCOMM_BUILD:=$PCOMM_ROOT/build}"

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
export PCOMM_PROVIDER_PATH="${PCOMM_PROVIDER_PATH:+$PCOMM_PROVIDER_PATH:}$PCOMM_BUILD/providers/ibverbs:$PCOMM_BUILD/providers/gpumem:$PCOMM_BUILD/providers/mlx5dv:$PCOMM_BUILD/providers/ucx"
# Providers link the shared engine (libpcomm_engine.so); they carry an $ORIGIN
# rpath to it, but export the dir too so a provider .so copied elsewhere (as the
# gpumem build does) still resolves it.
export LD_LIBRARY_PATH="$PCOMM_BUILD/libpcomm:${LD_LIBRARY_PATH:-}"

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
