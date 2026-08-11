#!/bin/bash
# Corpus scale capture — 64-rank NVSHMEM star over pure InfiniBand.
#
# NOT a suite case, for the same reason scale_nccl.sbatch is not one: the board
# runs on every change and a 16-node allocation is not something to spend on
# every board run. This job exists to produce ONE artifact — the widest capture
# we have, and the only one whose asymmetry is visible at scale.
#
#   sbatch tests/hpc/corpus/scale_hubspoke_ib.sbatch
#
# It is case 16's workload with exactly two variables changed — 4 ranks to 64,
# and P2P disabled so 4 GPUs per node is still an IB-only run — so anything that
# differs in the capture is a consequence of scale and not of a different
# program. Same binary, same pattern, same provider.
#
# WHY 16x4 AND NOT 64x1. Case 16 puts one rank per node because on one node
# NVSHMEM routes every put over NVLink and the NIC carries only bootstrap
# traffic; an ibverbs capture of that is empty of the pattern. NVSHMEM_DISABLE_P2P
# reaches the same end by the other route: it skips registering the P2P transport
# entirely (nvshmem/src/host/transport/transport.cpp gates on it), so intra-node
# peers fall back to the remote transport and every edge is verbs. That is the
# same trick scale_nccl already uses with NCCL_P2P_DISABLE=1, and it buys 64
# ranks for 16 nodes instead of 64. The tradeoff is real and worth naming: four
# ranks now share a node, an HCA and therefore a LID, so this capture leans on
# comm_edges keying peers by (lid, qp_num) rather than by qp_num alone — the
# collision class that produced impossible self-edges once already. At 64 ranks
# on 16 LIDs that key is under four times the pressure it faces in the suite.
#
# 16 nodes needs acc_ehpc: acc_debug is capped at 8 nodes and one submitted job,
# which would also serialise this behind whatever board run is in flight.
#
#SBATCH --job-name=pcomm-corpus-scale-hubspoke-ib
#SBATCH --account=etur02
#SBATCH --qos=acc_ehpc
#SBATCH --nodes=16
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:30:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

# Same arch-in-the-name discipline as cases 13-18: a cached binary from the
# other machine is not a build error, it is a silent wrong answer.
PCOMM_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
PCOMM_SM=${PCOMM_SM:-90}
APP="$HOME/asym_patterns-sm$PCOMM_SM"

if [ ! -e "$NVSHMEM_HOME/lib/libnvshmem_host.so" ]; then
  echo "!! no libnvshmem_host.so under $NVSHMEM_HOME/lib" >&2
  ls "$NVSHMEM_HOME/lib" 2>/dev/null | head -20 >&2
  exit 1
fi

if [ ! -x "$APP" ] || [ "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -nt "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_$PCOMM_SM,code=sm_$PCOMM_SM -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -Xlinker --disable-new-dtags -Xlinker -rpath -Xlinker "$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart \
    || { echo "!! nvcc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# Unconditionally, AFTER the build block, and LD_LIBRARY_PATH after that — the
# ordering case 16 documents at length: pcomm_load_build ends by reloading the
# runtime modules, which prepend a different NVSHMEM than the one the binary was
# just linked against.
pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"
echo "=== mpirun: $(command -v mpirun)"
echo "=== nvshmem this binary links against ==="
ldd "$APP" 2>/dev/null | grep -i nvshmem || echo "  (none reported by ldd)"

OUT=$(pcomm_out scale_hubspoke_ib); rm -rf "$OUT"; mkdir -p "$OUT"

# NVSHMEM_DISABLE_P2P=1 is the line that makes this an IB capture rather than an
# empty one. Without it 60 of the 126 edges are intra-node and go over NVLink,
# where an ibverbs provider cannot see them — the exact failure case 07 measured
# (8 PEs on 2 nodes: only the 2 hops that crossed the boundary appeared).
#
# -p ib only, and deliberately no p2p alongside: it has an open illegal-memory-
# access bug (PLAN-20260810-0854) that poisons the CUDA context and costs the
# capture. That is also why this run cannot cross-check itself against NVLink.
mpirun -np 64 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI -x NVSHMEM_DISABLE_P2P=1 \
  "$PCOMM" run -p ib -o "$OUT" -- "$APP" hub 4096 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"

# Case 16's two assertions, run against 64 ranks. Both are closed-form in
# params.ranks — the expected edge set is derived, not recorded — so they are
# the SAME check at 16x the width, and the params they read come from this
# directory's own meta.json (ranks: 64), not case 16's.
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/corpus/scale_hubspoke_ib"
