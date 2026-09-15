#!/bin/bash
# Case 20 — nccl_collectives: the NCCL host-API provider against a workload that
# opens every door it hooks.
#
# The suite already runs NCCL twice (02 inter-node over IB, 05 intra-node under
# NVBit), and neither says anything about the COLLECTIVE. Case 02 captures the
# verbs underneath it, case 05 captures GPU accesses that land in NCCL's own
# channel buffers and place in no object at all. Which collective ran, over
# which buffers, between which peers, was not in a capture anywhere.
#
# ONE NODE, and no NVBit. This case is about API coverage, so it is deliberately
# the cheapest thing in the suite: seconds, not the 481 s case 05 spends on
# NVBit processing NCCL's module. That is the point — a provider whose case is
# cheap gets run on every board.
#
#   sbatch tests/hpc/cases/20_nccl_collectives/job.sbatch
#
#SBATCH --job-name=pcomm-20-nccl-collectives
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_collectives-li"
if [ ! -x "$APP" ]; then                        # build once with the gcc-11 host
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_collectives.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out nccl_collectives); rm -rf "$OUT"; mkdir -p "$OUT"

# NCCL pinned the same way case 02 pins it, and for the same reason: without it
# NCCL picks channels and protocol from measured bandwidth, and anything derived
# from the capture drifts run to run. It matters less here than in 02 — this
# provider records the calls the application made, which NCCL's internal choices
# cannot change — but a case that is only repeatable by accident is not
# repeatable.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NCCL_DEBUG=WARN \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 \
  "$PCOMM" run -p nccl -o "$OUT" -- "$APP" 65536 2
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/20_nccl_collectives"
