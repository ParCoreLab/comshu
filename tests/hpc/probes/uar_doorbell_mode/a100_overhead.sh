#!/usr/bin/env bash
# What does watching the GPU doorbell cost the application?
#
# The du04 counterpart of mn5_overhead.sbatch, and the half that measurement
# could not reach. On MN5 the doorbell writer is a host thread and the "forward"
# is host -> host; there the forward measured free and the per-doorbell copy-out
# was the entire cost. The GPU hijack is a different shape: the GPU rings a
# fake page in HOST memory, a repeater thread wakes and replays the store to the
# NIC's BAR, so the forward really is GPU -> PCIe -> host -> PCIe -> BAR. That is
# the round trip PLAN-20260807-2344 exists to remove, and it has never been
# priced separately from the decode.
#
# Same four arms, same benchmark, same metric as MN5, so the two are comparable:
#
#   bare      no pcomm                                    the workload alone
#   passive   pcomm -p mlx5dv, no doorbell flag           control-plane hooks
#   forward   + --enable-doorbell-hijack,                 swap + repeater, NO copy-out
#             PCOMM_HIJACK_NO_DECODE=1
#   full      + --enable-doorbell-hijack                  everything
#
#   tests/hpc/probes/uar_doorbell_mode/a100_overhead.sh          # 3 reps
#   REPS=5 tests/hpc/probes/uar_doorbell_mode/a100_overhead.sh
#
# Both PEs sit behind ONE HCA here, so this is loopback rather than a fabric
# measurement — absolute bandwidth is not comparable to MN5's, but every arm
# sees the same loopback, so the RATIOS are what this is for.
set -uo pipefail

PCOMM_ROOT="${PCOMM_ROOT:-/DATA/missa18/pcomm}"
PCOMM="${PCOMM:-$PCOMM_ROOT/build-mlx5/pcomm/pcomm}"
export PCOMM_PROVIDER_PATH="${PCOMM_PROVIDER_PATH:-$PCOMM_ROOT/build-mlx5/providers/mlx5dv}"
SDK="${SDK:-/opt/nvidia/hpc_sdk/Linux_x86_64/2025/comm_libs/13.0/nvshmem}"
APP="${APP:-$SDK/bin/perftest/device/pt-to-pt/shmem_put_bw}"
export LD_LIBRARY_PATH="$SDK/lib:${LD_LIBRARY_PATH:-}"
WORK="${WORK:-${TMPDIR:-/tmp}/pcomm-a100-cost.$$}"
mkdir -p "$WORK"

# GPU-submitted IBGDA, which is what du04 can do and MN5 cannot. Same knobs
# case 12 uses for its gpu arm.
export NVSHMEM_BOOTSTRAP=MPI
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
export NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem

REPS=${REPS:-3}
RESULTS="$WORK/arms.csv"; echo "rep,arm,size,metric" > "$RESULTS"
MPI="mpirun -np 2 --allow-run-as-root -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH
     -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P -x NVSHMEM_REMOTE_TRANSPORT
     -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_IBGDA_NIC_HANDLER
     -x NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE -x PCOMM_HIJACK_NO_DECODE"

arm() {  # arm <name> <rep>
  # Two statements, not one: bash expands every word of a `local` command
  # before it assigns any of them, so "$name" in the same line is unbound under
  # `set -u`.
  local name="$1" rep="$2"
  local log="$WORK/log.$name.$rep" out="$WORK/cap.$name.$rep"
  rm -rf "$out"
  case "$name" in
    bare)    unset PCOMM_HIJACK_NO_DECODE; timeout 600 $MPI "$APP" >"$log" 2>&1 ;;
    passive) unset PCOMM_HIJACK_NO_DECODE
             timeout 600 $MPI "$PCOMM" run -p mlx5dv -o "$out" -- "$APP" >"$log" 2>&1 ;;
    forward) export PCOMM_HIJACK_NO_DECODE=1
             timeout 600 $MPI "$PCOMM" run -p mlx5dv --enable-doorbell-hijack \
               -o "$out" -- "$APP" >"$log" 2>&1 ;;
    full)    unset PCOMM_HIJACK_NO_DECODE
             timeout 600 $MPI "$PCOMM" run -p mlx5dv --enable-doorbell-hijack \
               -o "$out" -- "$APP" >"$log" 2>&1 ;;
  esac
  echo "-- $name rep$rep rc=$? --"
  grep -h "doorbell hijack:" "$log" 2>/dev/null | tail -2
  awk '/^[ \t]*[0-9]+[ \t]/ {print $1","$NF}' "$log" \
    | awk -F, -v r="$rep" -v a="$name" '{print r","a","$1","$2}' >> "$RESULTS"
}

echo "== host: $(hostname)  gpus: $(nvidia-smi -L | wc -l)  hca: $(ls /sys/class/infiniband | tr '\n' ' ') =="
for rep in $(seq 1 "$REPS"); do
  for a in bare passive forward full; do arm "$a" "$rep"; done
done

echo
echo "======== median GB/s per (arm, size), and cost vs bare ========"
python3 - "$RESULTS" <<'PY'
import csv, statistics, sys, collections
rows = list(csv.reader(open(sys.argv[1])))[1:]
d = collections.defaultdict(list)
for rep, arm, size, metric in rows:
    try: d[(arm, int(size))].append(float(metric))
    except ValueError: pass
arms = ["bare", "passive", "forward", "full"]
print(f"{'size':>9} " + " ".join(f"{a:>10}" for a in arms) + "   " +
      " ".join(f"{a+'/bare':>12}" for a in arms[1:]))
for s in sorted({k[1] for k in d}):
    med = {a: (statistics.median(d[(a, s)]) if d.get((a, s)) else None) for a in arms}
    base = med["bare"]
    cells = " ".join(f"{med[a]:10.2f}" if med[a] is not None else f"{'-':>10}" for a in arms)
    rat = " ".join(f"{med[a]/base:12.3f}" if (med[a] and base) else f"{'-':>12}" for a in arms[1:])
    print(f"{s:>9} {cells}   {rat}")
print()
print("forward/passive isolates the GPU->host->BAR round trip the on-GPU repeater")
print("would remove; full/forward isolates the per-doorbell copy-out. Check the")
print("hijack counters above: an arm that did not engage is not a cheap arm.")
PY
echo
echo "results: $RESULTS"
