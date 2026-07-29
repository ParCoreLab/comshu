#!/bin/bash
# What does tracing actually cost an ibverbs application?
#
# The format question turns on this denominator. A record append costs 12 ns
# raw and 37 ns with FlatBuffers (+25 ns), and post_send is on the app's
# critical path — but +25 ns only matters relative to what a post_send costs
# and how many records each message produces (we record BOTH post_send and
# poll_cq, so it is two appends per message).
#
# Measured with perftest against real NDR hardware, untraced vs traced, as a
# user would run it (through the pcomm launcher).
#SBATCH --job-name=pcomm-ibcost
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:25:00
#SBATCH --output=/home/koc/koc296797/ibcost.%j.out

PCOMM_ROOT="$HOME/proj/pcomm"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

NODES=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
SERVER=${NODES[0]}; CLIENT=${NODES[1]}
echo "server=$SERVER client=$CLIENT"

# Small messages + high iteration count: maximises message rate, which is the
# regime where a per-call software tax actually shows up. Large messages would
# hide it behind wire time and flatter the tracer.
SIZE=64
ITERS=200000

run_pair() {  # label  wrapper...
  local label="$1"; shift
  srun --nodes=1 --ntasks=1 -w "$SERVER" "$@" ib_send_bw -s $SIZE -n $ITERS -F --report_gbits \
      >$HOME/ibc-$label-srv.log 2>&1 &
  local spid=$!
  sleep 4
  srun --nodes=1 --ntasks=1 -w "$CLIENT" "$@" ib_send_bw -s $SIZE -n $ITERS -F --report_gbits "$SERVER" \
      >$HOME/ibc-$label-cli.log 2>&1
  wait $spid 2>/dev/null
  # perftest prints: bytes  iterations  BW peak  BW average  MsgRate(Mpps)
  local line=$(grep -E "^\s*$SIZE\s" $HOME/ibc-$label-cli.log | tail -1)
  local mpps=$(echo "$line" | awk '{print $NF}')
  printf 'IBCOST %-14s msg_rate=%-12s (%s)\n' "$label" "${mpps:-?}" "$(echo $line | tr -s ' ')"
}

echo "############ untraced baseline"
run_pair untraced

echo "############ traced with the ibverbs provider (raw-POD records, current)"
OUT=$HOME/ibc-out; rm -rf "$OUT"; mkdir -p "$OUT"
run_pair traced "$PCOMM" -p ib -o "$OUT" --

echo "############ records actually written (2 per message: post_send + poll_cq)"
for f in $(find "$OUT" -name '*.pcs' 2>/dev/null | sort); do
  printf '  %-52s %s bytes\n' "$f" "$(stat -c%s $f)"
done

echo "### ALL DONE"
