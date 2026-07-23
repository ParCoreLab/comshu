#!/bin/bash
# Case 02 — nccl_internode: a real NCCL all-reduce across two nodes with NVLink
# P2P disabled, so the entire collective rides InfiniBand and the `ib` provider
# sees the whole story. Exercises the at-scale path: GPUDirect MRs, hundreds of
# QPs, and post_send/poll_cq on NCCL's CPU proxy threads — i.e. cross-thread
# flush. This is the "inter-node, no p2p" half of the NCCL pair.
#
#   sbatch tests/hpc/cases/02_nccl_internode/job.sbatch
#
#SBATCH --job-name=pcomm-02-nccl-internode
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_allreduce"
if [ ! -x "$APP" ]; then                        # build once with the gcc-11 host
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out nccl_internode); rm -rf "$OUT"; mkdir -p "$OUT"

# NVLink P2P off ⇒ inter-node is pure IB; every rank wraps its app in `pcomm -p ib`.
mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=1 \
  "$PCOMM" -p ib -o "$OUT" -- "$APP" 8388608 20
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
python3 "$PCOMM_ROOT/tests/hpc/suite/check_capture.py" "$OUT" \
        "$PCOMM_ROOT/tests/hpc/cases/02_nccl_internode/expect.json"
