#!/bin/bash
# Smoke test for the public-UCP provider on the real MN5 MPI/UCX path.
# This intentionally records UCX intent only; ibverbs/UCT attribution is a
# separate experiment because MN5's mlx5 transport bypasses ibv_post_send.
#SBATCH --job-name=pcomm-ucx-provider
#SBATCH --account=etur02
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --qos=acc_debug
#SBATCH --output=ucx-provider-%j.out
#SBATCH --error=ucx-provider-%j.err
set -euo pipefail

ROOT="${PCOMM_ROOT:-$SLURM_SUBMIT_DIR}"
HERE="$ROOT/tests/hpc/probes/ucx_api_coverage"
BUILD="${PCOMM_BUILD:-$ROOT/build-ucx-provider-$SLURM_JOB_ID}"
OUT="${PCOMM_UCX_API_OUT:-$HERE/provider-$SLURM_JOB_ID}"
mkdir -p "$OUT"

module load gcc/13.2.0 cmake nvidia-hpc-sdk/25.7 >/dev/null 2>&1
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
MPI="$SDK/comm_libs/mpi"
# On MN5 comm_libs/mpi is the HPC-X launcher symlink; its UCX libraries live
# in the versioned HPC-X tree rather than beneath the symlink target.
UCX="$SDK/comm_libs/12.9/hpcx/hpcx-2.22.1/ucx"
export OMPI_CC=gcc
export LD_LIBRARY_PATH="/apps/ACC/GCC/13.2.0/lib64:$UCX/lib:${LD_LIBRARY_PATH:-}"

cmake -S "$ROOT" -B "$BUILD" -DPCOMM_BUILD_TESTS=OFF \
    -DPCOMM_PROVIDER_UCX=ON -DPCOMM_PROVIDER_IBVERBS=OFF \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DUCX_UCP_LIB="$UCX/lib/libucp.so" -DUCX_INCLUDE_DIR="$UCX/include" >/dev/null
cmake --build "$BUILD" -j 2 --target pcomm_launcher ucx_trace
"$MPI/bin/mpicc" -O2 -Wall -Wextra -Werror -o "$OUT/mpi_calls" "$HERE/mpi_calls.c"

export PCOMM_PROVIDER_PATH="$BUILD/providers/ucx"
export LD_LIBRARY_PATH="$BUILD/libpcomm:$LD_LIBRARY_PATH"

echo "=== UCX public-provider smoke ==="
echo "job: $SLURM_JOB_ID"
echo "nodes: $SLURM_JOB_NODELIST"
"$MPI/bin/mpirun" -np 2 --map-by ppr:1:node \
    -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
    "$BUILD/pcomm/pcomm" run -p ucx -o "$OUT/capture" -- "$OUT/mpi_calls"
