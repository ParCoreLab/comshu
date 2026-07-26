#!/bin/bash
# Case 04 — nvshmem: a NVSHMEM put-ring across 4 GPUs on one node. Device-side
# nvshmem_int_p stores to the next PE's symmetric heap travel over NVLink; the
# `p2p` (gpumem / NVBit) provider instruments those peer accesses and records
# the symmetric-heap allocations as MemObjects. Validates the PGAS path — the
# workload family Snoopie was built for.
#
# Needs the gpumem provider built on MN5 (NVBit). NB: on NVSHMEM 3.7 the heap
# fragments into many cuMemMap regions, so peer *attribution* may be off (see
# things-to-circle-back-to.md); this case asserts capture happened, not owner
# correctness.
#
#   sbatch tests/hpc/cases/04_nvshmem/job.sbatch
#
#SBATCH --job-name=pcomm-04-nvshmem
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

APP="$HOME/nvshmem_putget"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out nvshmem); rm -rf "$OUT"; mkdir -p "$OUT"

# PCOMM_KERNEL_NAME restricts instrumentation to our put kernel. NB the mechanism
# is not what it looks like: nvshmem_int_p is *inlined* into put_to_next, so the
# puts have no name of their own to filter on — they are captured precisely
# because they are part of put_to_next's SASS. What the filter excludes is
# NVSHMEM's own separately-launched kernels (barriers and friends), which are
# enormous and not byte-repeatable. Corollary worth knowing (nvbit_backend.cu:216
# tests each related function's name individually): a callee that is *not*
# inlined has its own name, won't match, and its accesses are silently dropped.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x PCOMM_KERNEL_NAME=put_to_next \
  "$PCOMM" -p p2p -o "$OUT" -- "$APP" 65536 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem"
