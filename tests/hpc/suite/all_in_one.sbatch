#!/bin/bash
# all_in_one.sbatch — run every suite case inside ONE allocation.
#
# Why: `acc_debug` permits one submitted job at a time, so the old
# submit → wait → submit loop paid a fresh queue wait plus SLURM and MPI startup
# for each of the eight cases. The workloads themselves total under ten minutes;
# the wall clock was ~25-30 min. This removes seven queue waits outright.
#
# The allocation is the union of what the cases ask for — 4 nodes x 4 tasks x
# 4 GPUs, whole-node CPU — and each case is executed exactly as it is submitted
# standalone. A case's job.sbatch is a plain bash script whose #SBATCH lines are
# comments, so running it under `bash` inside an allocation is the same program
# with the same srun/mpirun shapes; nothing is forked into a suite-only variant
# that could drift from what a lone `sbatch cases/NN/job.sbatch` does.
#
# IT WAS 2 NODES, AND THAT SILENTLY EXCLUDED THREE CASES. "The union of what the
# cases ask for" was true when written and stopped being true when 16-18 arrived
# asking for 4 nodes x 1 task (one GPU per node, so that every edge crosses the
# NIC). mpirun then refused to place 4 procs at ppr:1:node on 2 nodes —
# "this topology can support: Number of procs: 4, PPR: 1:node" — and the cases
# produced EMPTY capture directories whose only failing assertion was
# tables_present, with 17 skips. That reads exactly like a tracer regression and
# is not one. Measured twice: jobs 44688266 and its predecessor.
#
# 4 is still inside acc_debug's node=8 ceiling, so this costs no queue class.
#
# The union is now CHECKED rather than asserted in a comment — see the
# pre-flight below. A comment cannot notice that a new case outgrew it.
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
#SBATCH --nodes=4
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

# Two clocks, because job 44457178 lost an entire allocation to one case.
#
# Case 01's pingpong client could not reach its server ("Couldn't connect to
# as01r2b17:18515"); the case then sat in `wait` on a server that never exited,
# and SLURM killed the allocation at the time limit. Cases 02-12 never ran. The
# board was not red — it did not exist, which is worse, because a board that is
# merely absent looks like a scheduling problem rather than a defect.
#
# CASE_TIMEOUT bounds one case. It is not a performance budget: case 05 alone
# spends ~435 s lifting NCCL's modules under NVBit, so the cap has to sit well
# above the slowest legitimate case and only catch a hang.
#
# BUDGET bounds the suite, and exists so the driver stops on its own terms
# rather than being killed mid-case. A run that reports "10 of 12, out of time"
# is a result; one that SLURM terminates leaves no board at all. Default is the
# 40-minute allocation less five minutes for the check step and the tail.
CASE_TIMEOUT=${PCOMM_CASE_TIMEOUT:-900}
BUDGET=${PCOMM_SUITE_BUDGET:-2100}

# PRE-FLIGHT: does this allocation actually cover every case we were asked to
# run? Checked HERE, before a single workload starts, because the failure it
# catches is otherwise invisible until after the queue wait and then looks like
# a tracer bug (empty captures, tables_present the only red assertion).
#
# Each case declares its shape in its own #SBATCH lines, which are the same
# lines a standalone `sbatch` would honour — so this reads the case's real
# intent rather than a suite-local duplicate that could drift from it.
#
# HARD ERROR, not a skip. A case quietly dropped from the board is the failure
# mode this whole file already refuses for empty case lists: a green board that
# tested less than it claims. Whoever adds a case that outgrows the allocation
# must widen the allocation (or run it --per-case), and should be told so in one
# line instead of reading mpirun's topology complaint.
alloc_nodes=${SLURM_JOB_NUM_NODES:-0}
short=""
for c in $CASES; do
  j="$CASES_DIR/$c/job.sbatch"
  [ -f "$j" ] || continue
  want=$(sed -n 's/^#SBATCH --nodes=\([0-9]\+\).*/\1/p' "$j" | head -1)
  [ -n "$want" ] || continue
  if [ "$alloc_nodes" -gt 0 ] && [ "$want" -gt "$alloc_nodes" ]; then
    short="$short $c(needs ${want}n)"
  fi
done
if [ -n "$short" ]; then
  echo "!! allocation is ${alloc_nodes} nodes; these cases ask for more:$short" >&2
  echo "!! widen --nodes in $(basename "$0"), or run them with run_on_mn5.sh --per-case" >&2
  exit 2
fi

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
  left=$(( BUDGET - (SECONDS - suite_t0) ))
  if [ "$left" -le 60 ]; then
    echo "!! out of suite budget (${BUDGET}s) — not starting $c"
    printf '%-24s %6s %s\n' "$c" "-" "SKIPPED (out of budget)" >> "$TIMINGS"
    continue
  fi
  cap=$(( left < CASE_TIMEOUT ? left : CASE_TIMEOUT ))

  t0=$SECONDS
  # Own shell: every case exports its own LD_LIBRARY_PATH (NCCL vs NVSHMEM),
  # sets -x, and may `exit 1` on a workload failure. None of that may reach the
  # next case or this driver.
  #
  # -k 30: SIGTERM, then SIGKILL 30 s later. A case that hangs is usually
  # blocked in `wait` on an srun that will not exit, and TERM alone has already
  # been observed not to be enough.
  timeout -k 30 "$cap" bash "$job" > "$log" 2>&1
  rc=$?
  dt=$(( SECONDS - t0 ))
  # 124 is timeout's own code; 137 is SIGKILL, i.e. the -k grace also elapsed.
  case "$rc" in
    124|137) note="TIMEOUT after ${cap}s" ;;
    0)       note="" ;;
    *)       note="" ;;
  esac
  echo "<< $c   rc=$rc   ${dt}s ${note}"
  tail -5 "$log" | sed 's/^/   | /'
  printf '%-24s %6ss  rc=%s %s\n' "$c" "$dt" "$rc" "$note" >> "$TIMINGS"
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
