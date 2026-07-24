#!/bin/bash
# Case 03 — p2p_kernel: intra-node GPU-initiated peer read/write over NVLink,
# captured by the `p2p` (gpumem / NVBit) provider. A single process drives all
# GPUs with discrete peer-store kernels (NCCL's persistent collective kernels
# deadlock under NVBit — see things-to-circle-back-to.md). This is the
# intra-node counterpart to the inter-node IB case 02: same idea (who moves data
# to whom), opposite transport (NVLink vs IB), opposite provider (gpumem vs ib).
#
#   sbatch tests/hpc/cases/03_p2p_kernel/job.sbatch
#
#SBATCH --job-name=pcomm-03-p2p-kernel
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=80
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

APP="$HOME/p2p_kernel"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  nvcc -O2 -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/cases/03_p2p_kernel/p2p_kernel.cu" -o "$APP" -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out p2p_kernel); rm -rf "$OUT"; mkdir -p "$OUT"

# Single process, all GPUs → `-p p2p` runs in one pass (gpumem is exclusive but
# alone). 4096 elems × 4 iters of ring peer stores. PCOMM_RANK=0 pins the rank
# (no MPI here, so rank would otherwise fall back to pid → non-repeatable).
PCOMM_RANK=0 "$PCOMM" -p p2p -o "$OUT" -- "$APP" 4096 4
echo "app exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/03_p2p_kernel"
