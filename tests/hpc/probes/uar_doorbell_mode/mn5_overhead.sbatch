#!/bin/bash
# What does watching the host doorbell cost the application?
#
# The host path sits IN the data plane: it hands the transport a decoy page,
# a repeater thread forwards every doorbell store to the real register, and the
# decode copies the announced WQEs out of GPU memory. None of that is free, and
# until now the only number attached to it was the GPU hijack's "+68% forward,
# +86% forward+decode" — which uar_hijack.hpp itself flags as unusable, because
# it was wall-clock over a run whose startup dominated and therefore could not
# say which half the cost was in.
#
# Four arms, one allocation, one node pair, interleaved and repeated, measured
# with the APPLICATION's own steady-state numbers rather than wall-clock:
#
#   bare      no comshu at all                      the workload alone
#   passive   comshu -p mlx5dv, no doorbell flag    control-plane hooks only
#   forward   + --enable-host-doorbell,            decoy + repeater, NO copy-out
#             COMSHU_HIJACK_NO_DECODE=1
#   full      + --enable-host-doorbell             everything, WQEs decoded
#
# The differences are the decomposition:
#   passive - bare     = what the ordinary interposers cost
#   forward - passive  = the decoy page and the repeater's forward
#   full    - forward  = the per-doorbell cuMemcpyDtoHAsync + stream sync
#
# That last one is the number that decides whether the on-GPU repeater is worth
# building, which is the question COMSHU_HIJACK_NO_DECODE was added to answer and
# has never been used to answer.
#
#   sbatch --account=etur02 --qos=acc_debug \
#     tests/hpc/probes/uar_doorbell_mode/mn5_overhead.sbatch
#
#SBATCH --job-name=comshu-hd-cost
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:40:00
#SBATCH --output=hd_cost_%j.out
#SBATCH --error=hd_cost_%j.out

set -uo pipefail
: "${TMPDIR:=/tmp}"
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"

COMSHU="$COMSHU_ROOT/build/comshu/comshu"
export COMSHU_PROVIDER_PATH="$COMSHU_ROOT/build/providers/mlx5dv"
CUDART_DIR="$(dirname "$(ls "$COMSHU_SDK"/cuda/*/targets/x86_64-linux/lib/libcudart.so.12 2>/dev/null | head -1)")"
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib${CUDART_DIR:+:$CUDART_DIR}:${LD_LIBRARY_PATH:-}"
APP="$NVSHMEM_HOME/bin/perftest/device/pt-to-pt/shmem_put_bw"

export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=cpu
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_SYMMETRIC_SIZE=1g
export NVSHMEM_BOOTSTRAP=PMI
export NVSHMEM_BOOTSTRAP_PMI=PMI-2

SRUN="srun --mpi=pmi2 -n 2 --gpus-per-task=1 --overlap --export=ALL"
REPS=${REPS:-3}
RESULTS="$TMPDIR/arms.csv"; echo "rep,arm,size,metric" > "$RESULTS"

arm() {  # arm <name> <rep>
  local name="$1" rep="$2"
  local out="$TMPDIR/cost.$name.$rep"
  rm -rf "$out"
  local log="$TMPDIR/log.$name.$rep"

  case "$name" in
    bare)    env -u COMSHU_HIJACK_NO_DECODE $SRUN "$APP" > "$log" 2>&1 ;;
    passive) env -u COMSHU_HIJACK_NO_DECODE $SRUN "$COMSHU" run -p mlx5dv \
               -o "$out" -- "$APP" > "$log" 2>&1 ;;
    forward) COMSHU_HIJACK_NO_DECODE=1 $SRUN "$COMSHU" run -p mlx5dv \
               --enable-host-doorbell -o "$out" -- "$APP" > "$log" 2>&1 ;;
    full)    env -u COMSHU_HIJACK_NO_DECODE $SRUN "$COMSHU" run -p mlx5dv \
               --enable-host-doorbell -o "$out" -- "$APP" > "$log" 2>&1 ;;
  esac
  echo "-- $name rep$rep rc=$? --"
  grep -h "host doorbell:" "$log" 2>/dev/null | tail -2

  # shmem_put_bw prints "<size> <bw> <msgrate>"-shaped rows; keep the numeric
  # ones and record size + the LAST numeric column, whatever the build calls it.
  awk '/^[ \t]*[0-9]+[ \t]/ {print $1","$NF}' "$log" \
    | awk -F, -v r="$rep" -v a="$name" '{print r","a","$1","$2}' >> "$RESULTS"
}

echo "== nodes: $SLURM_JOB_NODELIST =="
for rep in $(seq 1 "$REPS"); do
  for a in bare passive forward full; do arm "$a" "$rep"; done
done

echo
echo "======== median metric per (arm, size), and cost vs bare ========"
python3 - "$RESULTS" <<'PY'
import csv, statistics, sys, collections
rows = list(csv.reader(open(sys.argv[1])))[1:]
d = collections.defaultdict(list)
for rep, arm, size, metric in rows:
    try: d[(arm, int(size))].append(float(metric))
    except ValueError: pass
sizes = sorted({s for _, s in d})
arms = ["bare", "passive", "forward", "full"]
print(f"{'size':>9} " + " ".join(f"{a:>12}" for a in arms) + "   " +
      " ".join(f"{a+'/bare':>13}" for a in arms[1:]))
for s in sizes:
    med = {a: (statistics.median(d[(a, s)]) if d.get((a, s)) else None) for a in arms}
    base = med["bare"]
    cells = " ".join(f"{med[a]:12.2f}" if med[a] is not None else f"{'-':>12}" for a in arms)
    ratio = " ".join(
        f"{med[a]/base:13.3f}" if (med[a] and base) else f"{'-':>13}" for a in arms[1:])
    print(f"{s:>9} {cells}   {ratio}")
print()
print("forward/passive isolates the repeater's forward; full/forward isolates the")
print("per-doorbell copy-out. A ratio near 1 on a column means that stage is free")
print("at this message size, not that the instrument is absent — check the")
print("doorbell counters printed above for that.")
PY
