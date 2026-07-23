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
#SBATCH --gres=gpu:4
#SBATCH --time=00:08:00
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

mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  "$PCOMM" -p p2p -o "$OUT" -- "$APP" 1048576 10
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
python3 "$PCOMM_ROOT/tests/hpc/suite/check_capture.py" "$OUT" \
        "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/expect.json"
