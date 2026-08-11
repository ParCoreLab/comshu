#!/bin/bash
# NCCL all-reduce over InfiniBand under the pcomm ibverbs provider — the
# at-scale test: a real collective drives NCCL's IB transport (GPUDirect MRs,
# many QPs, post_send/poll_cq on CPU proxy threads, captured via cross-thread
# flush). Reconstruct the inter-node comm graph from the capture.
#
# Build (MareNostrum 5; nvcc 12.9 front-end rejects gcc-13.2 libstdc++'s
# __is_convertible builtin, so compile with gcc-11.4 as host, run with
# gcc-13.2 libstdc++ available for the provider):
#   module load gcc/11.4.0 nvidia-hpc-sdk/25.7
#   SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
#   NCCL=$SDK/comm_libs/12.9/nccl-2.26
#   OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
#     tests/hpc/nccl_allreduce.cu -o ~/nccl_allreduce \
#     -I$NCCL/include -L$NCCL/lib -lnccl -lcudart
#
# Validated 2026-07-21 (SLURM 43613869, 2 nodes x 4 H100): 840 QPs, 57,280
# post_sends, 38,704 completions, 8 GPUDirect MRs, ~1.07 GB over IB. Inter-node
# graph reconstructs (ranks 0-3 node A <-> 4-7 node B); intra-node stays on
# NVLink (correctly not on the IB wire).
#
#   sbatch tests/hpc/nccl_allreduce.sbatch
#   pcomm export $OUT --csv $OUT-csv        # ib_modify_qp x ib_queue_pair x ib_post_send
#   pcomm export $OUT --pftrace nccl.pftrace  # timeline in ui.perfetto.dev
#
#SBATCH --job-name=pcomm-nccl
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
module load gcc/13.2.0 nvidia-hpc-sdk/25.7 >/dev/null 2>&1   # gcc-13.2 libstdc++ for the provider
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
export LD_LIBRARY_PATH=$SDK/comm_libs/12.9/nccl-2.26/lib:$LD_LIBRARY_PATH

LIB="${PCOMM_IB_LIB:-$HOME/proj/pcomm/build/providers/ibverbs/libibverbs_trace.so}"
PCOMM_BIN="${PCOMM_BIN:-$HOME/proj/pcomm/build/pcomm/pcomm}"
APP="${NCCL_APP:-$HOME/nccl_allreduce}"
OUT="$HOME/nccl-$SLURM_JOB_ID"; rm -rf "$OUT"; mkdir -p "$OUT"

# 8 ranks (4 GPUs x 2 nodes). Ranks come from OMPI env (distinct 0-7).
mpirun -np 8 --map-by ppr:4:node \
  -x PATH -x LD_LIBRARY_PATH \
  -x LD_PRELOAD="$LIB" -x PCOMM_OUTPUT_DIR="$OUT" \
  -x NCCL_DEBUG=WARN \
  "$APP" 8388608 20
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_BIN" read "$OUT"
