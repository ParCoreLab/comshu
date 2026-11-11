#!/bin/bash
# Case 02 — nccl_internode: a real NCCL all-reduce across two nodes with NVLink
# P2P disabled, so the entire collective rides InfiniBand and the `ib` provider
# sees the whole story. Exercises the at-scale path: GPUDirect MRs, hundreds of
# QPs, and post_send/poll_cq on NCCL's CPU proxy threads — i.e. cross-thread
# flush. This is the "inter-node, no p2p" half of the NCCL pair.
#
#   sbatch tests/hpc/cases/02_nccl_internode/job.sbatch
#
#SBATCH --job-name=comshu-02-nccl-internode
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_allreduce-li"
if [ ! -x "$APP" ]; then                        # build once with the gcc-11 host
  comshu_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -Wno-deprecated-gpu-targets \
    "$COMSHU_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  comshu_load_runtime
fi

OUT=$(comshu_out nccl_internode); rm -rf "$OUT"; mkdir -p "$OUT"

# NVLink P2P off ⇒ inter-node is pure IB; every rank wraps its app in `comshu run -p ib`.
# NCCL is pinned (Ring/Simple, fixed channels) so the capture is byte-repeatable
# — without this NCCL picks channel counts from measured bandwidth and the QP /
# send counts drift run-to-run, defeating the golden.
mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x COMSHU_PROVIDER_PATH -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=1 \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$COMSHU" run -p ib -o "$OUT" -- "$APP" 8388608 20
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/02_nccl_internode"
