#!/bin/bash
# Multinode validation of the pcomm ibverbs provider over real InfiniBand.
# Runs ibv_rc_pingpong across two nodes (server + client) under
# LD_PRELOAD=libibverbs_trace.so, into a shared output dir with explicit
# per-rank ids, then decodes the QP topology.
#
# Validated on MareNostrum 5 (BSC), NDR IB, 2026-07-21: each rank's modify_qp
# RTR row references the *other* node's QP+LID (distinct LIDs => genuine
# cross-node capture). Account/QoS below are MN5-specific; adjust per site.
#
#   sbatch tests/hpc/ib_multinode.sbatch
#
#SBATCH --job-name=pcomm-ib-mn
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:05:00
#SBATCH --output=%x.%j.out

set -x
module load gcc/13.2.0 >/dev/null 2>&1

LIB="${PCOMM_IB_LIB:-$HOME/proj/pcomm/build/providers/ibverbs/libibverbs_trace.so}"
PCOMM_BIN="${PCOMM_BIN:-$HOME/proj/pcomm/build/pcomm/pcomm}"
OUT="$HOME/ib-mn-$SLURM_JOB_ID"      # shared dir; per-rank subdirs
rm -rf "$OUT"; mkdir -p "$OUT"

nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server=${nodes[0]}; client=${nodes[1]}
echo "server=$server client=$client out=$OUT"

# Detect the active IB device per node, then run pingpong under the preload.
run='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}"); \
     echo "[$(hostname)] dev=$DEV"'

# rank 0 = server (listens), backgrounded
srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap \
     bash -c "$run; LD_PRELOAD=$LIB PCOMM_OUTPUT_DIR=$OUT PCOMM_RANK=0 \
              ibv_rc_pingpong -d \$DEV -n 20" &
SRV=$!
sleep 6

# rank 1 = client (connects to server hostname over OOB TCP)
srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap \
     bash -c "$run; LD_PRELOAD=$LIB PCOMM_OUTPUT_DIR=$OUT PCOMM_RANK=1 \
              ibv_rc_pingpong -d \$DEV -n 20 $server"
CLI_RC=$?
wait $SRV; SRV_RC=$?
echo "client_rc=$CLI_RC server_rc=$SRV_RC"

echo "=== captured streams ==="
"$PCOMM_BIN" read "$OUT"
echo "=== per-rank ib_queue_pair + ib_modify_qp (cross-node topology) ==="
for r in "$OUT"/rank-*; do
  echo "--- $r ---"
  echo "[queue_pair]"; "$PCOMM_BIN" dump "$r" 102
  echo "[modify_qp]";  "$PCOMM_BIN" dump "$r" 104
done
