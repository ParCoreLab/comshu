#!/bin/bash
#SBATCH --job-name=pcomm-ucx-nesting
#SBATCH --account=etur02
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --qos=acc_debug
#SBATCH --output=ucx-nesting-%j.out
#SBATCH --error=ucx-nesting-%j.err
#
# Does UCX post verbs work synchronously beneath a public UCP operation, later
# beneath worker_progress, or outside both scopes? The UCX preload probe exports
# its innermost scope; the gated diagnostic in the ibverbs provider counts the
# scope seen by every post_send work request.
set -euo pipefail

ROOT="${PCOMM_ROOT:-$SLURM_SUBMIT_DIR}"
HERE="$ROOT/tests/hpc/probes/ucx_api_coverage"
BUILD="${PCOMM_BUILD:-$ROOT/build-ucx-nesting-$SLURM_JOB_ID}"
OUT="${PCOMM_UCX_API_OUT:-$HERE/nesting-$SLURM_JOB_ID}"
mkdir -p "$OUT"

module load gcc/13.2.0 cmake nvidia-hpc-sdk/25.7 >/dev/null 2>&1
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
MPI="$SDK/comm_libs/mpi"
export OMPI_CC=gcc

CC=gcc CXX=g++ cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DPCOMM_BUILD_TESTS=OFF \
    -DPCOMM_PROVIDER_IBVERBS=ON >/dev/null
cmake --build "$BUILD" -j 2 --target pcomm ibverbs_trace
g++ -std=c++20 -shared -fPIC -O2 -Wall -Wextra -Werror \
    -o "$OUT/libucx_calls.so" "$HERE/ucx_calls.cpp" -ldl
"$MPI/bin/mpicc" -O2 -Wall -Wextra -Werror -o "$OUT/mpi_calls" "$HERE/mpi_calls.c"

export PCOMM_PROVIDER_PATH="$BUILD/providers/ibverbs"
export LD_LIBRARY_PATH="$BUILD/libpcomm:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="$OUT/libucx_calls.so"
export PCOMM_TRACE_UCX_NESTING=1

echo "=== UCX operation -> verbs nesting ==="
echo "job: $SLURM_JOB_ID"
echo "nodes: $SLURM_JOB_NODELIST"
"$MPI/bin/mpirun" -np 2 --map-by ppr:1:node \
    -x PATH -x LD_LIBRARY_PATH -x LD_PRELOAD \
    -x PCOMM_PROVIDER_PATH -x PCOMM_TRACE_UCX_NESTING \
    "$BUILD/pcomm/pcomm" run -p ib -o "$OUT/capture" -- "$OUT/mpi_calls"
