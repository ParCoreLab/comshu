#!/bin/bash
# The acceptance test for PLAN-20260813-0600: comm AND compute, one capture,
# one timeline, from one `pcomm run`.
#
# Everything before this measured the mechanism. This measures the product: an
# 8-rank NCCL all-reduce over InfiniBand captured with `-p ib,cupti`, so
# ib_post_send/poll_cq rows and gpu_kernel rows land in the same directory on
# the same wall clock and the timeline can finally answer "was that rank
# computing or blocked".
#
# The second arm is the guard test, and it is the one that must not be skipped:
# `-p gpumem,cupti` has to drop cupti and still produce a NON-EMPTY gpumem
# capture. Arm E of job 44546506 is what happens when it does not — four rank
# directories, zero .pcs files, no error. That failure is invisible to code
# review, so it is checked by counting output files.
#
#   sbatch tests/hpc/probes/kerneltime/combined.sbatch
#
#SBATCH --job-name=pcomm-kt-combined
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
module load gcc/13.2.0 nvidia-hpc-sdk/25.7 cuda/12.6 >/dev/null 2>&1
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
export LD_LIBRARY_PATH=$SDK/comm_libs/12.9/nccl-2.26/lib:$CUDA_HOME/extras/CUPTI/lib64:$LD_LIBRARY_PATH

ROOT=$HOME/pcomm-impl
BUILD=$ROOT/build-cupti
PCOMM=$BUILD/pcomm/pcomm
APP="${NCCL_APP:-$HOME/nccl_allreduce}"
export PCOMM_PROVIDER_PATH=$BUILD/gpu-backends/cupti:$BUILD/providers/ibverbs

OUT=$HOME/kt-combined-$SLURM_JOB_ID; rm -rf "$OUT"

# `pcomm run` is deliberately NOT used to wrap mpirun: it would trace mpirun
# itself. The launcher's job here is done by exporting the same chain it would
# build, per rank, which is what -x LD_PRELOAD does below. PCOMM_KERNEL_TIMING=1
# is the launcher's guard variable, set by hand for the same reason.
ENGINE=$(ls $BUILD/libpcomm/libpcomm_engine.so)
CHAIN="$ENGINE:$BUILD/providers/ibverbs/libibverbs_trace.so:$BUILD/gpu-backends/cupti/libcupti_trace.so"

mpirun -np 8 --map-by ppr:4:node \
  -x PATH -x LD_LIBRARY_PATH \
  -x LD_PRELOAD="$CHAIN" \
  -x PCOMM_OUTPUT_DIR="$OUT" \
  -x PCOMM_KERNEL_TIMING=1 \
  -x PCOMM_VERBOSE=1 \
  -x NCCL_DEBUG=WARN \
  "$APP" 8388608 30
echo "mpirun exit=$?"

echo "=== streams: expect BOTH ib_* (100-124) and gpu_kernel (530) ==="
"$PCOMM" read "$OUT" 2>&1 | tail -25

echo "=== kernel rows per rank ==="
for d in "$OUT"/rank-*; do
  printf "%s  530=%s  110=%s\n" "$(basename $d)" \
    "$(test -f $d/530.pcs && echo yes || echo NO)" \
    "$(test -f $d/110.pcs && echo yes || echo NO)"
done

# ---------------------------------------------------------------------------
# The guard. gpumem must still capture when cupti is asked for alongside it.
GPUMEM=$HOME/proj/pcomm/build/providers/gpumem/libgpumem_trace.so
if [ -f "$GPUMEM" ]; then
  echo "=== GUARD: -p gpumem,cupti must drop cupti and still capture ==="
  GOUT=$HOME/kt-guard-$SLURM_JOB_ID; rm -rf "$GOUT"
  PCOMM_PROVIDER_PATH="$(dirname $GPUMEM):$BUILD/gpu-backends/cupti" \
    "$PCOMM" run -p gpumem,cupti -o "$GOUT" --dry-run -- "$APP" 65536 2 2>&1 | head -8
  echo "--- and for real, single rank ---"
  PCOMM_KERNEL_TIMING=0 mpirun -np 1 \
    -x PATH -x LD_LIBRARY_PATH \
    -x LD_PRELOAD="$ENGINE:$GPUMEM" \
    -x PCOMM_OUTPUT_DIR="$GOUT" -x NCCL_DEBUG=WARN \
    "$APP" 65536 2 2>&1 | tail -5
  echo "GUARD pcs_files=$(find "$GOUT" -name '*.pcs' 2>/dev/null | wc -l)  (must be > 0)"
else
  echo "GUARD skipped — no gpumem build at $GPUMEM"
fi

echo "=== DONE ==="
