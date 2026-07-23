#!/bin/bash
# Case 03 — nccl_p2p: a NCCL all-reduce on a single node across 4 GPUs, with
# NVLink P2P forced ON. The traffic never touches IB — it is GPU-initiated peer
# read/write over NVLink, which only the `p2p` (gpumem / NVBit) provider can
# see. This is the "intra-node p2p" half of the NCCL pair, and the counterpart
# to case 02: same collective, opposite transport, opposite provider.
#
# Needs the gpumem provider built on MN5 (NVBit) — see suite/README.md.
#
#   sbatch tests/hpc/cases/03_nccl_p2p/job.sbatch
#
#SBATCH --job-name=pcomm-03-nccl-p2p
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
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_allreduce"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out nccl_p2p); rm -rf "$OUT"; mkdir -p "$OUT"

# Single node, P2P on ⇒ NVLink peer copies. `-p p2p` = gpumem provider (NVBit),
# exclusive but single-provider here, so it runs in one pass (no replay).
# NCCL pinned (Ring/Simple, fixed channels) for a byte-repeatable capture.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NCCL_DEBUG=WARN \
  -x NCCL_P2P_DISABLE=0 -x NCCL_P2P_LEVEL=NVL \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" -p p2p -o "$OUT" -- "$APP" 8388608 20
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/03_nccl_p2p"
