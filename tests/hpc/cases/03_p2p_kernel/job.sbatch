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
#SBATCH --job-name=comshu-03-p2p-kernel
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=80
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

APP="$HOME/p2p_kernel-li"
if [ ! -x "$APP" ]; then
  comshu_load_build
  nvcc -O2 -lineinfo -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$COMSHU_ROOT/tests/hpc/cases/03_p2p_kernel/p2p_kernel.cu" -o "$APP" -lcudart
  comshu_load_runtime
fi

OUT=$(comshu_out p2p_kernel); rm -rf "$OUT"; mkdir -p "$OUT"

# Single process, all GPUs → `-p p2p` runs in one pass (gpumem is exclusive but
# alone). 4096 elems × 4 iters of ring peer stores. COMSHU_RANK=0 pins the rank
# (no MPI here, so rank would otherwise fall back to pid → non-repeatable).
COMSHU_RANK=0 "$COMSHU" run -p p2p -o "$OUT" -- "$APP" 4096 4
echo "app exit=$?"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/03_p2p_kernel"
