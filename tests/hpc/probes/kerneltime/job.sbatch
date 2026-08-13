#!/bin/bash
# PROBE — what does engine-level kernel timing cost on a REAL workload?
#
# The microbenchmarks in this directory ran on shared login nodes and measured
# a pathological launch rate (11 us kernels back to back), so their overhead
# numbers are an upper bound taken under contention — du04 even reported
# NEGATIVE overhead, which is noise, not a speedup. This job exists to get the
# number that actually decides "can kernel timing be on by default": a real
# NCCL all-reduce, 8 ranks over IB, on dedicated nodes.
#
# Four arms, each the same workload:
#   A  bare                    — the baseline
#   B  + cupti_shim            — kernel timing alone
#   C  + ibverbs provider      — the capture we ship today
#   D  + ibverbs + cupti_shim  — what "kernel timing in the engine" would cost
#
# D-vs-C is the answer. B-vs-A isolates CUPTI from the provider's own cost.
#
#   sbatch tests/hpc/probes/kerneltime/job.sbatch
#
#SBATCH --job-name=pcomm-kerneltime
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:20:00
#SBATCH --output=%x.%j.out

set -x
module load gcc/13.2.0 nvidia-hpc-sdk/25.7 cuda/12.6 >/dev/null 2>&1
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
export LD_LIBRARY_PATH=$SDK/comm_libs/12.9/nccl-2.26/lib:$CUDA_HOME/extras/CUPTI/lib64:$LD_LIBRARY_PATH

LIB="${PCOMM_IB_LIB:-$HOME/proj/pcomm/build/providers/ibverbs/libibverbs_trace.so}"
PCOMM_BIN="${PCOMM_BIN:-$HOME/proj/pcomm/build/pcomm/pcomm}"
APP="${NCCL_APP:-$HOME/nccl_allreduce}"
SHIM="$HOME/pcomm-probe/cupti_shim.so"

# Build the shim here so the job is self-contained and always matches the source.
g++ -O2 -fPIC -shared -o "$SHIM" "$HOME/pcomm-probe/cupti_shim.cpp" \
    -I"$CUDA_HOME/extras/CUPTI/include" -L"$CUDA_HOME/extras/CUPTI/lib64" -lcupti || exit 1

SIZE=8388608
ITERS=50

run_arm() {
  local tag="$1"; shift
  local preload="$1"; shift
  local out="$1"; shift
  local extra=()
  [ -n "$out" ] && { rm -rf "$out"; mkdir -p "$out"; extra+=(-x PCOMM_OUTPUT_DIR="$out"); }
  echo "=== ARM $tag preload='$preload' ==="
  local t0=$(date +%s.%N)
  mpirun -np 8 --map-by ppr:4:node \
    -x PATH -x LD_LIBRARY_PATH \
    ${preload:+-x LD_PRELOAD="$preload"} \
    "${extra[@]}" \
    -x NCCL_DEBUG=WARN \
    "$APP" $SIZE $ITERS
  local rc=$?
  local t1=$(date +%s.%N)
  echo "ARM_RESULT tag=$tag rc=$rc wall_s=$(echo "$t1 - $t0" | bc)"
}

# A and B first, so the CUPTI-alone cost is measured without the provider in
# the process at all.
run_arm A ""                       ""
run_arm B "$SHIM"                  ""
run_arm C "$LIB"                   "$HOME/kt-C-$SLURM_JOB_ID"
# Order matters here and is the point: the engine/provider is preloaded FIRST,
# exactly as `pcomm run` builds its chain, and the shim rides behind it.
run_arm D "$LIB:$SHIM"             "$HOME/kt-D-$SLURM_JOB_ID"

echo "=== streams captured in arm C ==="; "$PCOMM_BIN" read "$HOME/kt-C-$SLURM_JOB_ID"
echo "=== streams captured in arm D ==="; "$PCOMM_BIN" read "$HOME/kt-D-$SLURM_JOB_ID"

# ---------------------------------------------------------------------------
# The coexistence claim, re-tested at scale rather than only on a login node.
# gpumem loads NVBit, which dlopens libcupti and takes the one subscriber slot,
# so the shim here MUST report CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED.
# A tiny message size and iteration count because NVBit's module lifting, not
# the workload, dominates this arm.
GPUMEM="$HOME/proj/pcomm/build/providers/gpumem/libgpumem_trace.so"
if [ -f "$GPUMEM" ]; then
  echo "=== ARM E: gpumem(NVBit) + cupti_shim — expect MULTIPLE_SUBSCRIBERS ==="
  mpirun -np 4 --map-by ppr:4:node -N 1 \
    -x PATH -x LD_LIBRARY_PATH \
    -x LD_PRELOAD="$GPUMEM:$SHIM" \
    -x PCOMM_OUTPUT_DIR="$HOME/kt-E-$SLURM_JOB_ID" \
    -x NCCL_DEBUG=WARN \
    "$APP" 65536 3 2>&1 | grep -E "SHIM|MULTIPLE|error" | head -20
else
  echo "ARM E skipped — no gpumem build at $GPUMEM"
fi

echo "=== ALL ARMS DONE ==="
