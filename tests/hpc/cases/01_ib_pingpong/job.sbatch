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
# ibv_rc_pingpong's server listens on 18515 only once it is actually up, and a
# fixed sleep is a guess about how long that takes. Job 44457178 lost the whole
# allocation to that guess being wrong: the client got "Couldn't connect to
# <host>:18515", exited 1, and the run then blocked forever in `wait` on a
# server that was still listening. So: poll, with a bound.
#
# OBSERVE the port, never connect to it. The first version of this poll used
# `exec 3<>/dev/tcp/127.0.0.1/18515` and broke the case in a new way (job
# 44457734): the server accepts exactly ONE connection and then expects a
# 52-byte address exchange on it, so the probe consumed the accept, sent
# nothing, and the server died with "0/52: Couldn't read remote address" before
# the real client ever ran. A readiness check that consumes the thing it is
# checking is worse than the sleep it replaced.
for _ in $(seq 1 30); do
  srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap \
    bash -c "ss -Hltn 'sport = :18515' | grep -q ." 2>/dev/null && break
  sleep 1
done

srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
  "$devcmd; PCOMM_RANK=1 '$PCOMM' -p ib -o '$OUT' -- ibv_rc_pingpong -d \$DEV -n 20 $server"
CLI=$?

# Reap the server on the client's terms. It exits by itself on a successful
# exchange; when the client failed it does not, and an unbounded `wait` turns
# one broken case into a dead allocation.
#
# The grace period is 60s and that is not padding. At 10s (job 44457851) the
# exchange SUCCEEDED — client_rc=0, 163840 bytes at 4325 Mbit/s — and the kill
# still fired, because the server was not hung, it was DRAINING its capture.
# server_rc=137 was my own SIGKILL, and it truncated the server rank's records,
# so four goldens and three assertions failed on a run where the workload had
# done exactly what it was asked. A backstop that fires on the happy path is not
# a backstop, it is a second bug wearing the first one's clothes.
#
# The driver's per-case timeout is the real bound on a hang; this only needs to
# be longer than a drain.
for _ in $(seq 1 60); do kill -0 "$SRV" 2>/dev/null || break; sleep 1; done
if kill -0 "$SRV" 2>/dev/null; then
  echo "!! server still running after client exited (client_rc=$CLI) — terminating"
  kill "$SRV" 2>/dev/null
  sleep 2
  kill -9 "$SRV" 2>/dev/null
fi
wait "$SRV"; SRV_RC=$?
echo "client_rc=$CLI server_rc=$SRV_RC"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/01_ib_pingpong"
