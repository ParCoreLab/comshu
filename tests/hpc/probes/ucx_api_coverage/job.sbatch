#!/bin/bash
#SBATCH --job-name=comshu-ucx-api
#SBATCH --account=etur02
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --qos=acc_debug
#SBATCH --output=ucx-api-%j.out
#SBATCH --error=ucx-api-%j.err
#
# Stage 0 of PLAN-20260815-1540: measure which public UCP calls MN5's real MPI
# path makes. This intentionally builds a disposable preload counter, not the
# provider, so an absent hook becomes evidence before it becomes an empty table.
set -euo pipefail

ROOT="${COMSHU_ROOT:-$SLURM_SUBMIT_DIR}"
HERE="$ROOT/tests/hpc/probes/ucx_api_coverage"
OUT="${COMSHU_UCX_API_OUT:-$HERE/out-$SLURM_JOB_ID}"
mkdir -p "$OUT"

module load gcc/13.2.0 nvidia-hpc-sdk/25.7 >/dev/null 2>&1
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
MPI="$SDK/comm_libs/mpi"
export OMPI_CC=gcc

g++ -std=c++20 -shared -fPIC -O2 -Wall -Wextra -Werror \
    -I/usr/include -o "$OUT/libucx_calls.so" "$HERE/ucx_calls.cpp" -ldl
"$MPI/bin/mpicc" -O2 -Wall -Wextra -Werror -o "$OUT/mpi_calls" "$HERE/mpi_calls.c"

echo "=== UCX API coverage stage 0 ==="
echo "job: $SLURM_JOB_ID"
echo "nodes: $SLURM_JOB_NODELIST"
ucx_info -v | head -4
"$MPI/bin/mpirun" -np 2 --map-by ppr:1:node \
    -x LD_PRELOAD="$OUT/libucx_calls.so" \
    -x LD_LIBRARY_PATH \
    "$OUT/mpi_calls"
