#!/bin/bash
# Case 05 — nccl_p2p: NCCL all-reduce over NVLink under the gpumem/NVBit provider.
#
# Was xfail from 2026-07-21 to 2026-07-29, filed as a deadlock caused by "NCCL's
# persistent kernel starving NVBit's bounded channel". That was wrong in every
# part. It is not a deadlock — it completes — and the channel is empty during
# the slow phase.
#
# The cost is a FIXED ~435s of NVBit module processing, and it is fixed in the
# strongest sense: measured at N=100, 1024, 16384, 65536 and 262144, every one
# takes 433-443s while the capture scales faithfully from 0.6 MB to 47 MB
# (jobs 43971039, 43972064). Uninstrumented is 6s. NCCL ships 223 device-function
# specializations / 434k instructions in one module and NVBit processes the
# module, not the kernel we ask for. Confirmed not reachable from our side:
# restricting instrumentation to one kernel changes nothing (438s vs 436s), and
# nor does skipping the per-launch arming of unfiltered functions.
#
# So the deadline reflects the tool, not the workload. Do NOT "fix" this by
# shrinking the message — that was tried across a 2621x range and does nothing.
#
#SBATCH --job-name=pcomm-05-nccl-p2p
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:20:00
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

OUT=$(pcomm_out nccl_p2p); rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT" "$OUT.csv"
DEADLINE=600   # ~435s of fixed NVBit module cost + headroom

timeout "$DEADLINE" mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NCCL_DEBUG=WARN \
  -x NCCL_P2P_DISABLE=0 -x NCCL_P2P_LEVEL=NVL \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" run -p p2p -o "$OUT" -- "$APP" 262144 3
rc=$?
echo "workload rc=$rc"
if [ $rc -ne 0 ]; then
  echo "FAILED: rc=$rc (124 = exceeded ${DEADLINE}s)"
  mkdir -p "$OUT.csv"
  printf '{"case":"05_nccl_p2p","ok":false,"checks":0,"rc":%d}\n' "$rc" > "$OUT.csv/result.json"
  exit 1
fi

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/05_nccl_p2p"
