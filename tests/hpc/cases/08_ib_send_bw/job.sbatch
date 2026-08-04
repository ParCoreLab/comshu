#!/bin/bash
# Case 08 — ib_send_bw: perftest over real IB, which uses rdma-core's *new*
# work-request API (ibv_wr_start / ibv_wr_send / ibv_wr_complete on ibv_qp_ex).
#
# This is the case that had no coverage and needed it most. Measured 2026-07-30
# (job 43976008), ib_send_bw traced end to end produced 202,000 ib_poll_cq
# records and ZERO ib_post_send: the wr_* calls are static inline wrappers over
# function pointers inside qp_ex, so they never reach ctx->ops.post_send, which
# is the only thing the old hook could see. Every send was invisible, and
# nothing failed — the capture just quietly had no sends in it.
#
# Case 01 (ibv_rc_pingpong) cannot catch this: it uses the classic
# ibv_post_send, so it passes either way. The two cases together are what
# distinguish "sends are captured" from "sends are captured for one API".
#
#   sbatch tests/hpc/cases/08_ib_send_bw/job.sbatch
#
#SBATCH --job-name=pcomm-08-ib-send-bw
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:05:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

# The oracle: -n is the iteration count perftest posts, -s the message size.
# Both are asserted directly by the goldens, so this case fails if the WR-API
# path drops, duplicates or mis-sizes a single work request.
ITERS=1000
SIZE=8192

OUT=$(pcomm_out ib_send_bw); rm -rf "$OUT"; mkdir -p "$OUT"
nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server=${nodes[0]}; client=${nodes[1]}
echo "server=$server client=$client out=$OUT iters=$ITERS size=$SIZE"

devcmd='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

# rank 0 = server (receives), rank 1 = client (posts the sends).
srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap bash -c \
  "$devcmd; PCOMM_RANK=0 '$PCOMM' -p ib,devx -o '$OUT' -- ib_send_bw -d \$DEV -n $ITERS -s $SIZE" &
SRV=$!
sleep 6
srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
  "$devcmd; PCOMM_RANK=1 '$PCOMM' -p ib,devx -o '$OUT' -- ib_send_bw -d \$DEV -n $ITERS -s $SIZE $server"
CLI=$?; wait $SRV; SRV_RC=$?
echo "client_rc=$CLI server_rc=$SRV_RC"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/08_ib_send_bw"
