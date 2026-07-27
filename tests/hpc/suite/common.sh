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
export PCOMM="$PCOMM_BUILD/pcomm/pcomm"
export PCOMM_READ="$PCOMM_BUILD/pcomm-read/pcomm-read"
export PCOMM_PROVIDER_PATH="$PCOMM_BUILD/providers/ibverbs:$PCOMM_BUILD/providers/gpumem"
# Providers link the shared engine (libpcomm_engine.so); they carry an $ORIGIN
# rpath to it, but export the dir too so a provider .so copied elsewhere (as the
# gpumem build does) still resolves it.
export LD_LIBRARY_PATH="$PCOMM_BUILD/libpcomm:${LD_LIBRARY_PATH:-}"

# NVIDIA HPC SDK 25.7 = CUDA 12.9 + NCCL 2.26 + NVSHMEM + HPC-X MPI.
: "${PCOMM_SDK:=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7}"
export NCCL_HOME="$PCOMM_SDK/comm_libs/12.9/nccl-2.26"
export NVSHMEM_HOME="$PCOMM_SDK/comm_libs/12.9/nvshmem"

# Runtime toolchain: the providers need gcc-13.2 libstdc++ (GLIBCXX_3.4.31);
# compile CUDA with gcc-11.4 host (nvcc 12.9 rejects gcc-13.2's __is_convertible).
pcomm_load_runtime() { module load gcc/13.2.0 nvidia-hpc-sdk/25.7 >/dev/null 2>&1; }
pcomm_load_build()   { module load gcc/11.4.0 nvidia-hpc-sdk/25.7 >/dev/null 2>&1; }

# Where a case drops its capture + where the checker reads it.
pcomm_out() { echo "${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/$1-${SLURM_JOB_ID:-local}"; }
