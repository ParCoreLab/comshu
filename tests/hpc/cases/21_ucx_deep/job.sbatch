#!/bin/bash
# Case 21 — ucx_deep: every UCT protocol, on hardware, in one allocation.
#
# WHY THIS CASE EXISTS. Case 19 proves the public UCP doors are reached. Job
# 45113099 proved the UCT layer beneath them is reachable too — 295 operations
# through the `uct_iface_ops_t` table — but every one of those was an active
# message. Three of the four op families in ucx_deep_records.hpp had no hardware
# evidence at all: zero-copy was four rows, rendezvous was zero, hardware tag
# matching was zero. Their record comments describe fields (`iovcnt`, `tag`,
# `remote_addr`, the advertised-vs-transferred distinction) that nothing has
# ever populated, and a field nobody has seen written is a field whose meaning
# nobody has checked.
#
# UCX chooses its protocol BY SIZE against thresholds, so this case PINS the
# thresholds and sweeps the sizes rather than hoping the defaults happen to
# straddle them. Pinning is not tuning: with the stack's own defaults the run
# would still be a valid capture and would still tell us nothing new, which is
# how job 45113099 came back all-active-messages.
#
# TWO ARMS IN ONE ALLOCATION, because MN5 allows one job at a time
# account-wide and the contrast is the finding. Only the first is checked by
# sql_check.sh — assertions have no per-arm granularity (tests/hpc/apps/
# RERUN-2026-08-27.md records that gap) — and the second is printed for the
# record, where "tag doors: 0 with TM off, N with TM on" is a measurement of the
# knob rather than of the tracer.
#
#   sbatch tests/hpc/cases/21_ucx_deep/job.sbatch
#
#SBATCH --job-name=comshu-21-ucx_deep
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:20:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

# ONE RANK PER NODE, case 19's reason unchanged: UCX picks shared memory for a
# same-node pair, and a shm-only run would exercise the doors without ever
# reaching the rc/dc transports whose protocol choices this case is about.
MPI_BIN="$(dirname "$(command -v mpirun)")"
APP="$HOME/uct_path_coverage"
SRC="$COMSHU_ROOT/tests/hpc/probes/uct_path_coverage/size_sweep.c"

if [ ! -x "$APP" ] || [ "$SRC" -nt "$APP" ]; then
  "$MPI_BIN/mpicc" -O2 -Wall -Wextra -Werror -o "$APP" "$SRC" \
    || { echo "!! mpicc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# The UCX tree belonging to the MPI THAT WILL ACTUALLY RUN. The provider
# interposes public UCP symbols and patches a UCT ops table read out of the
# loaded library; build against one libucp and run against another and the
# interposition silently does not engage. comshu_find_ucx is the one definition,
# shared with run_on_mn5.sh's build.
UCX_HOME_RESOLVED="$(comshu_find_ucx)" || {
  echo "!! no HPC-X UCX tree found beside this MPI ($(command -v mpirun))" >&2
  exit 1; }
UCX_LIB="$UCX_HOME_RESOLVED/lib"
[ -e "$UCX_LIB/libucp.so" ] || { echo "!! no libucp.so under $UCX_LIB" >&2; exit 1; }
export LD_LIBRARY_PATH="$UCX_LIB:$LD_LIBRARY_PATH"

echo "=== ucx this run resolves ==="
ls -la "$UCX_LIB/libucp.so"* 2>/dev/null | head -3
"$UCX_HOME_RESOLVED/bin/ucx_info" -v 2>/dev/null | head -3

# THE THRESHOLDS, and each one names the family it is there to reach. They are
# spelled here and repeated in meta.json's "workload" block, which is what makes
# an assertion able to say "a message of msg_rndv_bytes must have taken the
# rendezvous door" rather than restating whatever the run produced.
#
#   ZCOPY_THRESH  2048   -> 4096 B and up register the user buffer and DMA from
#                           it, which is what fills `iovcnt`.
#   RNDV_THRESH   32768  -> 65536 B and up advertise instead of sending, which
#                           is the ONLY door where `payload` is bytes that did
#                           NOT leave this node.
#   TM_THRESH     1024   -> everything above it is eligible for the ConnectX tag
#                           matcher, which is the only source of `uct_send.tag`.
#
# TM_ENABLE is set for both mlx5 RC and DC because which of the two UCX selects
# is its decision, not ours, and setting only the one it did not pick would
# produce a capture with no tag doors and no way to tell that from a tracer bug.
UCX_ARM_ENV=(
  -x UCX_ZCOPY_THRESH=2048
  -x UCX_RNDV_THRESH=32768
  -x UCX_TM_THRESH=1024
  -x UCX_RC_MLX5_TM_ENABLE=y
  -x UCX_DC_MLX5_TM_ENABLE=y
)

# UCX_TLS IS DELIBERATELY NOT SET. Forcing a transport list is the fastest way
# to turn a capture into a failed run on a fabric whose device set we do not
# control, and `uct_iface` records which transports UCX actually opened — so
# what it chose is a measurement this capture carries, not an assumption this
# script has to make.

# The output directory is computed by the CALLER and passed in, rather than
# echoed back out of the function. A function that returns a path through stdout
# has to be called in a subshell, and every diagnostic it prints — `comshu read`,
# the mpirun exit status, the export's own report — is then captured instead of
# logged. That is the whole value of the job output file, swallowed to avoid one
# variable.
run_arm() {   # <out-dir> <extra -x flags...>
  local out="$1"; shift
  rm -rf "$out"; mkdir -p "$out"
  mpirun -np 2 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH \
    -x COMSHU_PROVIDER_PATH --mca osc ucx "$@" \
    "$COMSHU" run -p ucx,ucx_deep -o "$out" -- "$APP"
  echo "arm=$(basename "$out") mpirun exit=$?"
  echo "=== captured streams ($out) ==="; "$COMSHU" read "$out"
  "$COMSHU" export "$out" --csv "$out.csv"
  comshu_derive "$out"
}

# --mca osc ucx: MPI_Put/MPI_Get reach UCT's Put*/Get* doors only through the
# UCX one-sided component. With osc/rdma or osc/pt2pt they become active
# messages and `remote_addr` — the object-heatmap key the whole HPC-X class has
# never had — is never written by anything.

# THE PRIMARY ARM IS NAMED FOR THE CASE, not for the arm, and that is a harness
# requirement rather than a preference: run_on_mn5.sh aggregates the board by
# looking for `<case-without-its-NN_-prefix>-<jobid>.csv`. Naming this one
# `ucx_deep_tm_on` cost a full queue slot — job 45114566 ran correctly, passed
# every assertion it had, and came back as `21_ucx_deep ---- no result` because
# the aggregator was looking for a directory nobody had written.
PRIMARY="$(comshu_out ucx_deep)"
run_arm "$PRIMARY" "${UCX_ARM_ENV[@]}"

# The contrast arm: same thresholds, tag offload OFF. Everything else identical,
# so a difference in the tag doors is attributable to the knob and to nothing
# else. Not checked by sql_check.sh — printed, for the record.
CONTRAST="$(comshu_out ucx_deep_tm_off)"
run_arm "$CONTRAST" \
  -x UCX_ZCOPY_THRESH=2048 -x UCX_RNDV_THRESH=32768 -x UCX_TM_THRESH=1024 \
  -x UCX_RC_MLX5_TM_ENABLE=n -x UCX_DC_MLX5_TM_ENABLE=n

echo "=== the two arms, side by side ==="
for a in "$PRIMARY" "$CONTRAST"; do
  echo "--- $a"
  if [ -d "$a.pq" ]; then
    "$COMSHU" sql "$a.pq" \
      "SELECT scope, tl_name, op, sum(ops) AS ops, sum(out_bytes) AS out_bytes,
              sum(in_bytes) AS in_bytes, sum(advertised_bytes) AS advertised
       FROM transport_mix GROUP BY 1,2,3 ORDER BY 1,2,3" 2>&1
    # severity included: on this case the row that matters most is
    # uct_ops_patch_refused, and a refusal printed without the word 'defect'
    # beside it is what let a correct guard read as an empty capture for two
    # days. See link_sql.hpp's severity paragraph.
    "$COMSHU" sql "$a.pq" \
      "SELECT finding, severity, n FROM link_health WHERE n > 0
       ORDER BY severity, finding" 2>&1
  else
    echo "   (no derived layer for this arm — the raw uct_send counts follow)"
    # rank,ticks,op,... — the CSV export prepends `rank` to Table<UctSend>::cols,
    # which itself begins "ticks,op". So the op name is field 3, not field 2.
    awk -F, 'NR>1{c[$3]++} END{for (o in c) print o, c[o]}' "$a.csv"/uct_send.csv 2>/dev/null
  fi
done

bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$PRIMARY.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/21_ucx_deep"
