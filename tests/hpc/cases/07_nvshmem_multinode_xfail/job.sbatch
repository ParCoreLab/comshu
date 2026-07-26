#!/bin/bash
# Case 07 — nvshmem_multinode (XFAIL): the unified NVLink+IB *traffic* capture.
# 8 PEs over 2 nodes, both providers in one process:
#
#   PE0→PE1→PE2→PE3 --IB--> PE4→PE5→PE6→PE7 --IB--> PE0
#     \_____ NVLink _____/    \_____ NVLink _____/
#
# Six ring hops are intra-node (NVLink, seen by gpumem/NVBit); two cross the node
# boundary (NVSHMEM's IBRC proxy posts real verbs, seen by ibverbs). One
# execution, so cross-transport causality is recoverable — which replay can never
# give, its passes being different runs.
#
# KEPT RED ON PURPOSE (project policy: don't weaken a major use case to go
# green). Multinode NVSHMEM deadlocks under NVBit. Isolated 2026-07-27, and the
# isolation matters because the obvious suspect was wrong:
#
#   1 node  + BOTH providers  -> exit 0    (coexistence is fine)
#   2 nodes + gpumem ALONE    -> exit 124  (hangs without ib in the picture)
#   2 nodes + NO_EAGER_LOAD=0 -> exit 124  (NVBit's own deadlock hint no help)
#
# So this is NOT a coexistence bug; it is the same family as case 05 — a comm
# library spinning on the device vs NVBit's bounded channel. 06_nvshmem_coexist
# is the passing stand-in for the coexistence property itself.
#
# `timeout 300` bounds the hang so the job self-reports (rc=124) instead of
# burning its wall clock, exactly as case 05 does.
#
#   sbatch tests/hpc/cases/07_nvshmem_multinode_xfail/job.sbatch
#
#SBATCH --job-name=pcomm-07-nvshmem-multinode-xfail
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:12:00
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

# Pin the inter-node transport to IBRC (proxy-based): its WQEs are posted by a
# host CPU thread through libibverbs, which is what the `ib` provider hooks.
# IBGDA would have the GPU post WQEs directly, bypassing host verbs entirely —
# the IB half of this capture would silently come back empty rather than fail.
export NVSHMEM_REMOTE_TRANSPORT=ibrc
export NVSHMEM_IB_ENABLE_IBGDA=0

OUT=$(pcomm_out nvshmem_multinode_xfail); rm -rf "$OUT"; mkdir -p "$OUT"

# PCOMM_KERNEL_NAME restricts NVBit instrumentation to our put kernel; without it
# NVBit instruments all of NVSHMEM's internal device code (spin/retry loops),
# which is enormous and not byte-repeatable. See case 04.
timeout 300 mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x NVSHMEM_REMOTE_TRANSPORT -x NVSHMEM_IB_ENABLE_IBGDA \
  -x PCOMM_KERNEL_NAME=put_to_next \
  "$PCOMM" -p ib,p2p -o "$OUT" -- "$APP" 65536 3
rc=$?; echo "mpirun exit=$rc"
# Self-report the expected outcome so run_suite can classify XFAIL vs XPASS.
if [ $rc -eq 124 ]; then outcome=deadlock; else outcome=completed; fi
mkdir -p "$OUT.csv"
printf '{"case":"07_nvshmem_multinode_xfail","outcome":"%s","rc":%d}\n' "$outcome" "$rc" \
  > "$OUT.csv/result.json"
[ $rc -eq 124 ] && exit 0

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/07_nvshmem_multinode_xfail"
