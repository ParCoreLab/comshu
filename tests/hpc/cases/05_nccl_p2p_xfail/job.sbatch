#!/bin/bash
# Case 05 — nccl_p2p  [KNOWN-FAILING / xfail]
#
# A single-node NCCL all-reduce over NVLink under the `p2p` (gpumem / NVBit)
# provider. This is a MAJOR use case we want to support and DELIBERATELY keep as
# a test even though it currently DEADLOCKS: NCCL runs a persistent collective
# kernel that occupies the GPU, so NVBit's bounded device→host channel can never
# flush → producer/consumer deadlock (NCCL inits, prints its version, hangs).
# Case 03 (discrete p2p_kernel) is the passing stand-in; this case stays red on
# purpose so the limitation stays visible until we fix it (e.g. a spillable /
# unbounded channel, or cooperative flush that preempts the persistent kernel).
#
# Bounded by `timeout` so the deadlock can't burn the wall clock. The job writes
# its own result.json; run_suite classifies it XFAIL (expected) — or XPASS (an
# alert to remove this marker) if it ever completes.
#
#SBATCH --job-name=pcomm-05-nccl-p2p-xfail
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

APP="$HOME/nccl_allreduce"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out nccl_p2p_xfail); rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT" "$OUT.csv"
DEADLINE=180   # seconds; a healthy run finishes in a few

timeout "$DEADLINE" mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NCCL_DEBUG=WARN \
  -x NCCL_P2P_DISABLE=0 -x NCCL_P2P_LEVEL=NVL \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" -p p2p -o "$OUT" -- "$APP" 262144 3
rc=$?
echo "workload rc=$rc (124 = timed out = deadlock reproduced)"

# Classify and self-report. Completed (rc 0 + a capture) would be an XPASS.
if [ "$rc" -eq 0 ] && ls "$OUT"/rank-*/700.pcs >/dev/null 2>&1; then
  outcome=completed
else
  outcome=deadlock
fi
printf '{"case":"05_nccl_p2p_xfail","expect_fail":true,"outcome":"%s","rc":%d}\n' \
       "$outcome" "$rc" > "$OUT.csv/result.json"
echo "outcome=$outcome  (see meta.json: expect_fail)"
cat "$OUT.csv/result.json"
