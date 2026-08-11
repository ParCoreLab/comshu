#!/bin/bash
# Multinode comm-graph test for the pcomm ibverbs provider. Builds a 4-node
# ring of ibv_rc_pingpong pairs (client on node i connects to node (i+1)%4)
# under LD_PRELOAD=libibverbs_trace.so, into one shared output dir. The reader
# reconstructs the ring from the modify_qp->RTR topology (keyed on LID, the HCA
# identity) weighted by post_send bytes.
#
# Validated on MareNostrum 5 (BSC), NDR IB, 2026-07-21: 4 HCAs, 8 RC queue
# pairs, ring 4396-8227-7748-8165-4396, 200 KiB/direction/edge (50x4096 B).
#
# Each process gets a distinct PCOMM_RANK (server i, client i+4) so the shared
# dir holds one rank-* subtree per process. NOTE: without the explicit rank,
# srun --ntasks=1 steps all report SLURM_PROCID=0 and collide into rank-0 (the
# LID-keyed graph still reconstructs, but the per-rank split is lost).
#
#   sbatch tests/hpc/ib_ring.sbatch
#   pcomm export $OUT --csv $OUT-csv   # then feed ib_modify_qp.csv x ib_post_send.csv
#
#SBATCH --job-name=pcomm-ib-ring
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=2
#SBATCH --time=00:05:00
#SBATCH --output=%x.%j.out

set -x
module load gcc/13.2.0 >/dev/null 2>&1
LIB="${PCOMM_IB_LIB:-$HOME/proj/pcomm/build/providers/ibverbs/libibverbs_trace.so}"
PCOMM_BIN="${PCOMM_BIN:-$HOME/proj/pcomm/build/pcomm/pcomm}"
OUT="$HOME/ib-ring-$SLURM_JOB_ID"
rm -rf "$OUT"; mkdir -p "$OUT"
nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
echo "ring over: ${nodes[*]}  out=$OUT"

DEV='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

# One server per node (rank i).
for i in 0 1 2 3; do
  srun --nodes=1 --ntasks=1 --nodelist="${nodes[i]}" --overlap \
       bash -c "$DEV; LD_PRELOAD=$LIB PCOMM_OUTPUT_DIR=$OUT PCOMM_RANK=$i \
                ibv_rc_pingpong -d \$DEV -n 50" &
done
sleep 8
# Client on node i connects to node (i+1)%4 -> ring edge i -> i+1 (rank i+4).
for i in 0 1 2 3; do
  nxt=$(((i + 1) % 4))
  srun --nodes=1 --ntasks=1 --nodelist="${nodes[i]}" --overlap \
       bash -c "$DEV; LD_PRELOAD=$LIB PCOMM_OUTPUT_DIR=$OUT PCOMM_RANK=$((i + 4)) \
                ibv_rc_pingpong -d \$DEV -n 50 ${nodes[nxt]}" &
done
wait

echo "=== streams ==="; "$PCOMM_BIN" read "$OUT"
echo "=== csv export ==="; "$PCOMM_BIN" export "$OUT" --csv "$OUT-csv"
echo "=== ring topology (modify_qp -> RTR: local qp @ its LID -> dest qp @ dest LID) ==="
awk -F, 'NR>1 && $7==2 && $6!=0 {print "  qp "$4" -> dest qp "$5" @ LID "$6}' "$OUT-csv/ib_modify_qp.csv"
