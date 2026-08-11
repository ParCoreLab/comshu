#!/bin/bash
# Corpus scale capture — 32-rank NCCL all-reduce over pure InfiniBand.
#
# NOT a suite case, and deliberately not in tests/hpc/cases/. The suite board
# runs on every change and a 8-node allocation is not something to spend on
# every board run; this job exists to produce ONE artifact — a capture big
# enough and wide enough to size the UI's derived tables against. See
# tests/hpc/corpus/README.md.
#
#   sbatch tests/hpc/corpus/scale_nccl.sbatch
#
# It is case 02's workload with exactly one variable changed — 2 nodes to 8 —
# so anything that differs in the capture is a consequence of scale and not of
# a different program. Same binary, same NCCL pinning, same provider.
#
# 8 nodes is not a round number, it is the ceiling: acc_debug is capped at
# node=8 (MaxWall 02:00:00, MaxSubmitPU 1). 64 ranks would need acc_ehpc, which
# is a different queue with a different wait, so 32 is the largest capture
# reachable on the debug QoS at all.
#
#SBATCH --job-name=pcomm-corpus-scale-nccl
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:30:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_allreduce"
if [ ! -x "$APP" ]; then                        # build once with the gcc-11 host
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT=$(pcomm_out scale_nccl); rm -rf "$OUT"; mkdir -p "$OUT"

# Identical pinning to case 02. Without it NCCL picks channel counts from
# measured bandwidth, and at 32 ranks that choice varies across the allocation
# as well as across runs — which would make this capture unrepeatable and
# therefore useless as a sizing reference.
mpirun -np 32 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=1 \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" run -p ib -o "$OUT" -- "$APP" 8388608 20
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"

# The question this job exists to answer, printed in the job log so it is
# available without fetching 32 ranks of capture first.
#
# comm_edges resolves a send's QP to a peer rank through (src_lid, qp_num).
# That key is exact within one HCA; across 8 nodes it is the first real test of
# whether it is exact across an allocation. A correct 32-rank Ring is 32 edges
# forward and 32 back, no self-edges, and no dst_rank = -1 — anything else is
# the finding, and it is a finding about the reader, not about NCCL.
echo "=== comm_edges (expect a 32-node ring, no self-edges, no -1) ==="
cat "$OUT.csv/comm_edges.csv"
echo "=== edge health ==="
awk -F, 'NR>1 { n++; if ($1 == $2) self++; if ($2 == -1) unres++ }
         END { printf "edges=%d self_edges=%d unresolved=%d\n", n, self+0, unres+0 }' \
    "$OUT.csv/comm_edges.csv"

# No sql_check.sh run here, deliberately. It is driven by a case directory whose
# meta.json `workload` block is the oracle, and case 02's says ranks=8 — pointing
# it at this capture would fail on the oracle rather than on the capture, which
# is worse than not running it. The edge-health line above is the check that is
# actually meaningful at this scale, and it needs no oracle: a ring is a ring at
# any rank count.
#
# If this capture becomes a permanent fixture rather than a sizing artifact, it
# earns its own case directory with its own workload block. That is a decision
# to make after seeing the numbers, not before.
