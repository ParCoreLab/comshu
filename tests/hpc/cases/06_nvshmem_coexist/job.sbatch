#!/bin/bash
# Case 06 — nvshmem_coexist: the coexistence regression test. Case 04's exact
# workload and topology (1 node, 4 PEs, NVSHMEM put-ring), with the single
# variable changed: **both providers loaded into one process**.
#
# gpumem is no longer `exclusive` in the launcher registry (commit 7a5b55a), so
# `-p ib,p2p` plans ONE pass instead of two replay passes, and both .so land in
# the same LD_PRELOAD. Both write into one recorder, one rank space, one
# timeline. If the weak-symbol interposition bug ever returns, this case dies at
# startup rather than silently degrading — and no other case would catch it,
# since every other case loads exactly one provider.
#
# SCOPE, honestly stated: on one node every peer is NVLink-reachable, so NVSHMEM
# builds its IB transport but never posts a send. The ib side contributes
# entities (mem_region / queue_pair / modify_qp), NOT traffic — ib_post_send and
# ib_poll_cq are legitimately empty here. Cross-transport *traffic* correlation
# needs 2 nodes, which currently deadlocks under NVBit: see
# 07_nvshmem_multinode.
#
#   sbatch tests/hpc/cases/06_nvshmem_coexist/job.sbatch
#
#SBATCH --job-name=pcomm-06-nvshmem-coexist
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

# Same workload binary as case 04 — the ring spans whatever PE count it is given,
# so the only difference here is 8 PEs across 2 nodes instead of 4 on one.
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

OUT=$(pcomm_out nvshmem_coexist); rm -rf "$OUT"; mkdir -p "$OUT"

# PCOMM_KERNEL_NAME as in case 04 — and see that case's note: the puts are
# inlined into put_to_next (so they have no name of their own); what the filter
# excludes is NVSHMEM's separately-launched internal kernels.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x PCOMM_KERNEL_NAME=put_to_next \
  "$PCOMM" run -p ib,p2p -o "$OUT" -- "$APP" 65536 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/06_nvshmem_coexist"
