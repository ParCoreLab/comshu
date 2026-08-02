#!/bin/bash
# all_in_one.sbatch — run every suite case inside ONE allocation.
#
# Why: `acc_debug` permits one submitted job at a time, so the old
# submit → wait → submit loop paid a fresh queue wait plus SLURM and MPI startup
# for each of the eight cases. The workloads themselves total under ten minutes;
# the wall clock was ~25-30 min. This removes seven queue waits outright.
#
# The allocation is the union of what the cases ask for — 2 nodes x 4 tasks x
# 4 GPUs, whole-node CPU — and each case is executed exactly as it is submitted
# standalone. A case's job.sbatch is a plain bash script whose #SBATCH lines are
# comments, so running it under `bash` inside an allocation is the same program
# with the same srun/mpirun shapes; nothing is forked into a suite-only variant
# that could drift from what a lone `sbatch cases/NN/job.sbatch` does.
#
#   PCOMM_CASES       space-separated case dirs to run (default: all, in order)
#   PCOMM_CASES_FILE  same, one per line — how run_on_mn5.sh passes it, since
#                     `--export` splits on commas and the ssh wrapper on spaces
#   PCOMM_ROOT        repo on the remote
#   PCOMM_OUT_ROOT    where captures land
#
#SBATCH --job-name=pcomm-suite
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:40:00
#SBATCH --output=%x.%j.out

PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
export PCOMM_ROOT
CASES_DIR="$PCOMM_ROOT/tests/hpc/cases"
OUT_ROOT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}"
export PCOMM_OUT_ROOT="$OUT_ROOT"
mkdir -p "$OUT_ROOT"

if [ -n "${PCOMM_CASES:-}" ]; then
  CASES="$PCOMM_CASES"
elif [ -n "${PCOMM_CASES_FILE:-}" ] && [ -r "$PCOMM_CASES_FILE" ]; then
  CASES="$(cat "$PCOMM_CASES_FILE")"
else
  CASES="$(for d in "$CASES_DIR"/*/; do basename "$d"; done)"
fi
[ -n "$CASES" ] || { echo "!! empty case list — refusing to report an empty green board" >&2; exit 2; }

echo "=== pcomm suite, one allocation ==="
echo "job      : ${SLURM_JOB_ID:-none}"
echo "nodes    : ${SLURM_JOB_NODELIST:-?}"
echo "cases    : $CASES"
echo "out root : $OUT_ROOT"
echo "started  : $(date -Is)"

TIMINGS="$OUT_ROOT/timings-${SLURM_JOB_ID:-local}.txt"
: > "$TIMINGS"
suite_t0=$SECONDS

for c in $CASES; do
  job="$CASES_DIR/$c/job.sbatch"
  if [ ! -f "$job" ]; then
    echo "!! no such case: $c" >&2
    printf '%-24s %6s %s\n' "$c" "-" "MISSING" >> "$TIMINGS"
    continue
  fi
  log="$OUT_ROOT/$c-${SLURM_JOB_ID:-local}.log"
  echo
  echo "---------------------------------------------------------------"
  echo ">> $c   (log: $log)"
  t0=$SECONDS
  # Own shell: every case exports its own LD_LIBRARY_PATH (NCCL vs NVSHMEM),
  # sets -x, and may `exit 1` on a workload failure. None of that may reach the
  # next case or this driver.
  ( bash "$job" ) > "$log" 2>&1
  rc=$?
  dt=$(( SECONDS - t0 ))
  echo "<< $c   rc=$rc   ${dt}s"
  tail -5 "$log" | sed 's/^/   | /'
  printf '%-24s %6ss  rc=%s\n' "$c" "$dt" "$rc" >> "$TIMINGS"
done

suite_dt=$(( SECONDS - suite_t0 ))
echo
echo "=== per-case job time (queue wait excluded — that is the driver's to report) ==="
cat "$TIMINGS"
printf '%-24s %6ss\n' "TOTAL (in-job)" "$suite_dt" | tee -a "$TIMINGS"
echo "finished : $(date -Is)"

# The driver's `check` step decides PASS/FAIL from each case's result.json. This
# script exits 0 even when a case failed, on purpose: a non-zero exit here would
# say "the allocation failed", which is a different claim and would mask which
# case it was. A case that never ran leaves no result dir and reads as red.
exit 0
