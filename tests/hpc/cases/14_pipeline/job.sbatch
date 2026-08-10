#!/bin/bash
# Case 14 — pipeline: NVSHMEM chain, PE i -> i+1 only, volume growing along the chain
#
# Shares tests/hpc/asym_patterns.cu with the other two asymmetric-pattern cases;
# only the pattern argument differs. See that file for why these exist — briefly,
# every capture in the corpus was a ring or a pair, so every comm matrix was
# symmetric and every object heatmap was flat, and neither of those could be
# told apart from a bug.
#
#   sbatch tests/hpc/cases/14_pipeline/job.sbatch
#
#SBATCH --job-name=pcomm-14-pipeline
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

# Compute capability from the device rather than hardcoded, because these three
# cases are meant to run on MN5's H100s (sm_90) AND on the A100 box (sm_80),
# where run_on_a100.sh executes this file directly. A fixed -gencode is why the
# older cases are MN5-only.
# PCOMM_SM, not CC: `CC` is the C compiler variable, and pcomm_load_build below
# loads modules that export it. The first version used CC and was clobbered
# BETWEEN its two uses — the binary was named asym_patterns-sm90 (correct, read
# before the modules loaded) while -gencode became
# arch=compute_/apps/.../bin/nvc (read after). nvcc rejected it and the case
# then ran a binary that did not exist, exit 127.
PCOMM_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
PCOMM_SM=${PCOMM_SM:-90}
APP="$HOME/asym_patterns-sm$PCOMM_SM"

# One binary per arch for all three cases, so the three captures differ only in
# the pattern and never in the build. The arch is in the name because a cached
# binary from the other machine is not a build error, it is a silent wrong
# answer.
if [ ! -x "$APP" ] || [ "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -nt "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_$PCOMM_SM,code=sm_$PCOMM_SM -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart \
    || { echo "!! nvcc failed — refusing to run a stale or missing binary" >&2; exit 1; }
  pcomm_load_runtime
fi

OUT=$(pcomm_out pipeline); rm -rf "$OUT"; mkdir -p "$OUT"

# PCOMM_KERNEL_NAME=put_slice for the same reason as case 04: nvshmem_int_p is
# inlined into put_slice and captured as part of its SASS, and the filter's job
# is to exclude NVSHMEM's own separately-launched barrier kernels, which are
# enormous and not byte-repeatable.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x PCOMM_KERNEL_NAME=put_slice \
  "$PCOMM" -p p2p -o "$OUT" -- "$APP" pipe 4096 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/14_pipeline"
