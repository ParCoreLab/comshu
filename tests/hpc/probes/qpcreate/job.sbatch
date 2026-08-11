#!/bin/bash
# PROBE — which interposer does perftest actually ENTER to create its queue pair?
#
# Not a suite case. It answers one question and then either becomes a hook or
# gets deleted.
#
# Case 08 captures a queue pair being driven (5 ib_modify_qp rows) and never
# being created (ib_queue_pair and mlx5_queue_pair both empty), while the ib
# provider is healthy in the same process (1000 post_send, 1010 poll_cq).
#
# The previous revision of this probe used LD_DEBUG=bindings. That was the wrong
# instrument and it is worth recording why, because three explanations were
# asserted from its output and all three were wrong. Bindings show which
# definition a symbol RESOLVES to, not which symbol is CALLED. In job 44205459
# both rdma_create_qp and rdma_create_qp_ex resolved, and perftest cannot be
# calling both. So bindings proved "if this is called it reaches us" — for every
# door — and could not narrow further.
#
# PCOMM_TRACE_HOOKS=1 measures entry instead, at invoke_preload/invoke_object_ops
# in the hook engine, so it covers EVERY hooked symbol in both providers at once
# rather than a hand-picked three. It prints the first entry per symbol and
# whether a callback was listening, which separates the two indistinguishable
# failure modes: a door we never hooked, versus a hook nobody registered on.
#
#SBATCH --job-name=pcomm-probe-qpcreate
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-qpcreate-${SLURM_JOB_ID:-local}"
rm -rf "$OUT"; mkdir -p "$OUT"

nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server="${nodes[0]}"; client="${nodes[1]}"
devcmd='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

# Traced on both sides: the client is the active connector, but a create path
# taken only by the passive side would otherwise be invisible, and that asymmetry
# is exactly the kind of thing this probe exists to not assume.
srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap bash -c \
  "$devcmd; PCOMM_TRACE_HOOKS=1 PCOMM_RANK=0 '$PCOMM' run -p ib,devx -o '$OUT/cap' \
   -- ib_send_bw -d \$DEV -n 20 -s 1024" >"$OUT/server.log" 2>&1 &
sleep 4
srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
  "$devcmd; PCOMM_TRACE_HOOKS=1 PCOMM_RANK=1 '$PCOMM' run -p ib,devx -o '$OUT/cap' \
   -- ib_send_bw -d \$DEV -n 20 -s 1024 $server" >"$OUT/client.log" 2>&1
wait

echo "=== every interposer entered, both ranks ==="
grep -h 'pcomm-trace' "$OUT"/server.log "$OUT"/client.log | sort -u

echo "=== of those, the create-qp doors specifically ==="
if grep -h 'pcomm-trace' "$OUT"/server.log "$OUT"/client.log | grep -E 'create_qp|devx_obj_create'; then
  :
else
  # This is a real outcome, not an error: it means the queue pair is built by
  # something outside our hook set entirely. Do not add a hook on this alone —
  # widen the trace (the engine already covers every hooked symbol, so the next
  # step is ltrace/gdb on the create path, not a guess).
  echo "  NONE — no create door was entered by either rank"
fi

echo "=== positive control: the trace itself is working ==="
# Without this, "no create door" and "the probe printed nothing at all" look
# identical. post_send is known to fire ~1000 times on this workload.
grep -hc 'pcomm-trace' "$OUT"/server.log "$OUT"/client.log

echo "=== did either provider write a queue-pair entity? ==="
find "$OUT/cap" -name '*.pcs' | sed 's|.*/||' | sort | uniq -c

echo "=== what the binary imports (static, for comparison) ==="
readelf --dyn-syms --wide "$(command -v ib_send_bw)" 2>/dev/null |
  grep -oE "(ibv|mlx5dv|rdma)_create_qp[a-z_]*@[A-Z0-9_.]+" | sort -u
