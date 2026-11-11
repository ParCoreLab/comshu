#!/bin/bash
# Case 04 — nvshmem: a NVSHMEM put-ring across 4 GPUs on one node. Device-side
# nvshmem_int_p stores to the next PE's symmetric heap travel over NVLink; the
# `p2p` (gpumem / NVBit) provider instruments those peer accesses and records
# the symmetric-heap allocations as MemObjects. Validates the PGAS path — the
# workload family Snoopie was built for.
#
# Needs the gpumem provider built on MN5 (NVBit). Peer attribution is exact
# again as of 2026-08-03: heap ownership comes from nvshmem_ptr rather than from
# the number and order of the cuMemMap regions the heap happens to fragment
# into, so this case asserts the ring, not just that capture happened.
#
#   sbatch tests/hpc/cases/04_nvshmem/job.sbatch
#
#SBATCH --job-name=comshu-04-nvshmem
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

APP="$HOME/nvshmem_putget-li-$(sha1sum "$COMSHU_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" | cut -c1-10)"
if [ ! -x "$APP" ]; then
  comshu_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -rdc=true \
    -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$COMSHU_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart
  comshu_load_runtime
fi

# AFTER the build block, not before it, and this line has already cost a job.
#
# NVSHMEM_HOME is globbed to the NEWEST SDK (26.5 on MN5) while
# comshu_load_build/comshu_load_runtime do `module load nvidia-hpc-sdk/25.7` — and
# each of those PREPENDS 25.7's lib dir. Setting this before the build block
# therefore puts 26.5 first, the build block shoves 25.7 in front of it, and the
# loader hands the binary a libnvshmem_host it was not linked against:
# "undefined symbol: nvshmem_selected_device_transport, version NVSHMEM",
# exit 127, no capture at all.
#
# This case survived it only because its binary was cached, so the build block
# never ran — exactly as 13_hubspoke's comment predicted it would fail "the day
# someone changes the workload". Adding -lineinfo was that day (job 44696379).
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

OUT=$(comshu_out nvshmem); rm -rf "$OUT"; mkdir -p "$OUT"

# COMSHU_KERNEL_NAME restricts instrumentation to our put kernel. NB the mechanism
# is not what it looks like: nvshmem_int_p is *inlined* into put_to_next, so the
# puts have no name of their own to filter on — they are captured precisely
# because they are part of put_to_next's SASS. What the filter excludes is
# NVSHMEM's own separately-launched kernels (barriers and friends), which are
# enormous and not byte-repeatable. Corollary worth knowing (nvbit_backend.cu:216
# tests each related function's name individually): a callee that is *not*
# inlined has its own name, won't match, and its accesses are silently dropped.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x COMSHU_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x COMSHU_KERNEL_NAME=put_to_next \
  "$COMSHU" run -p p2p -o "$OUT" -- "$APP" 65536 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/04_nvshmem"
