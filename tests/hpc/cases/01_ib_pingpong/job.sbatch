#!/bin/bash
# Case 01 — ib_pingpong: the baseline. Two nodes run ibv_rc_pingpong (server +
# client) over real NDR InfiniBand under `pcomm -p ib`. Proves the hook engine
# still turns live verbs calls into records: MR / QP / modify_qp / post_send /
# poll_cq on both endpoints. Smallest end-to-end signal that the tool works.
#
#   sbatch tests/hpc/cases/01_ib_pingpong/job.sbatch
#   (the job itself runs sql_check.sh against checks/*.sql goldens)
#
#SBATCH --job-name=pcomm-01-ib-pingpong
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

OUT=$(pcomm_out ib_pingpong); rm -rf "$OUT"; mkdir -p "$OUT"
nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server=${nodes[0]}; client=${nodes[1]}
echo "server=$server client=$client out=$OUT"

devcmd='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

# rank 0 = server (listens), backgrounded; rank 1 = client (connects OOB).
srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap bash -c \
  "$devcmd; PCOMM_RANK=0 '$PCOMM' -p ib -o '$OUT' -- ibv_rc_pingpong -d \$DEV -n 20" &
SRV=$!
sleep 6
srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
  "$devcmd; PCOMM_RANK=1 '$PCOMM' -p ib -o '$OUT' -- ibv_rc_pingpong -d \$DEV -n 20 $server"
CLI=$?; wait $SRV; SRV_RC=$?
echo "client_rc=$CLI server_rc=$SRV_RC"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/01_ib_pingpong"
