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
#SBATCH --job-name=comshu-08-ib-send-bw
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:05:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

# The oracle: -n is the iteration count perftest posts, -s the message size.
# Both are asserted directly by the goldens, so this case fails if the WR-API
# path drops, duplicates or mis-sizes a single work request.
ITERS=1000
SIZE=8192

OUT=$(comshu_out ib_send_bw); rm -rf "$OUT"; mkdir -p "$OUT"
nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server=${nodes[0]}; client=${nodes[1]}
echo "server=$server client=$client out=$OUT iters=$ITERS size=$SIZE"

devcmd='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

# rank 0 = server (receives), rank 1 = client (posts the sends).
#
# THIS USED TO BE `sleep 6` AND IT LOST AN ALLOCATION TWICE. Measured on jobs
# 45411974 and 45417843 — two failures in six runs, both identical:
#
#     + SRV=2576608
#     + sleep 6
#     + srun ... --nodelist=as01r5b19 ...      <- the SERVER's trace, AFTER the sleep
#     Couldn't connect to as01r5b19:18515
#
# The server runs in a background subshell, so its own `set -x` line appears
# only when that subshell is scheduled — after the parent finished sleeping.
# Six seconds went to SLURM step launch plus LD_PRELOAD and two-provider init,
# and ib_send_bw had not reached listen(). The suite's driver already documents
# this exact symptom on case 01 ("Couldn't connect to as01r2b17:18515"); it is a
# recurring race in the perftest cases, not a novelty.
#
# WAIT ON AN OBSERVED FACT. `$OUT/rank-0` is created by FileSink when the
# server's Tracer starts, which happens in the LD_PRELOAD constructor — after
# SLURM has launched the step and the providers are up, before ib_send_bw binds.
# That removes the step-launch latency, which is the part that actually varies;
# the short grace after it covers only preload -> listen().
#
# NOT A PORT PROBE, deliberately. ib_send_bw's server accept()s exactly once, so
# a TCP probe to 18515 would be consumed AS the client connection and break the
# run it was meant to make reliable.
#
# AND THE CLIENT'S FAILURE MUST NOT COST 900 s. `wait $SRV` on a server blocked
# in accept() never returns, so a one-second connect failure became the case
# timeout — which is what starved cases 19-24 on job 45411974. Kill the server
# and retry the pair instead.
run_pair() {
  rm -rf "$OUT"; mkdir -p "$OUT"
  srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap bash -c \
    "$devcmd; COMSHU_RANK=0 '$COMSHU' run -p ib,devx -o '$OUT' -- ib_send_bw -d \$DEV -n $ITERS -s $SIZE" &
  SRV=$!
  for _ in $(seq 1 60); do
    [ -d "$OUT/rank-0" ] && break
    # The server step dying early must not be waited out for a minute.
    kill -0 "$SRV" 2>/dev/null || break
    sleep 1
  done
  sleep 3
  srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
    "$devcmd; COMSHU_RANK=1 '$COMSHU' run -p ib,devx -o '$OUT' -- ib_send_bw -d \$DEV -n $ITERS -s $SIZE $server"
  CLI=$?
  if [ "$CLI" -ne 0 ]; then
    kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV_RC=-1
    return 1
  fi
  wait "$SRV"; SRV_RC=$?
  return 0
}

for attempt in 1 2 3; do
  echo "=== attempt $attempt ==="
  run_pair && break
  echo "!! attempt $attempt failed to establish the perftest connection (client_rc=$CLI)"
  [ "$attempt" -lt 3 ] && sleep 5
done
echo "client_rc=$CLI server_rc=$SRV_RC attempts=$attempt"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/08_ib_send_bw"
