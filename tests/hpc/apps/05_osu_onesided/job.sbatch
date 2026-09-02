#!/bin/bash
# 05_osu_onesided — the OSU one-sided benchmarks against pcomm's remote-key and
# one-sided doors.
#
# WHAT THIS JOB IS FOR. `ucx_rkey` (626) and `ucx_rkey_pack` (628) are new, and
# every one-sided capture this project has ever taken was of a workload the
# pcomm authors wrote themselves. OSU is third-party code with an EXACT oracle:
# the number of one-sided operations, their size, their direction and the rank
# that issues them all follow from four command-line arguments and twenty lines
# of osu_put_bw.c. PREDICTION.md was committed before this job was ever
# submitted; meta.json holds the arithmetic.
#
# BUILD FIRST, ON THE LOGIN NODE: build_osu.sh. This script refuses to run if
# the binaries are not there, rather than burning a slot to discover it.
#
#   bash tests/hpc/apps/05_osu_onesided/build_osu.sh   # login node, once
#   while squeue -u koc296797 -h | grep -q .; do sleep 60; done
#   sbatch tests/hpc/apps/05_osu_onesided/job.sbatch
#
#SBATCH --job-name=pcomm-05-osu_onesided
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:25:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
STUDY="$PCOMM_ROOT/tests/hpc/apps/05_osu_onesided"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

OSU="${OSU:-$HOME/osu-7.5/libexec/osu-micro-benchmarks/mpi/one-sided}"
for b in osu_put_bw osu_get_bw osu_acc_latency osu_fop_latency; do
  [ -x "$OSU/$b" ] || { echo "!! $OSU/$b missing — run build_osu.sh on the login node first" >&2; exit 1; }
done

# ---------------------------------------------------------------------------
# THE ARGUMENTS ARE THE ORACLE. Do not change one without changing meta.json.
#
#   -m 512:8192  five sizes, doubling. Starts at 512 so no MPI control word can
#                collide with a workload size and the per-size row count is an
#                exact filter (meta.json says why at length).
#   -W 8         eight MPI_Puts per iteration, inside one lock/unlock.
#   -i 20 -x 5   sets iterations AND iterations_large, skip AND skip_large
#                (osu_util.c:360, :326), so the large-message re-set at
#                osu_put_bw.c:296 cannot change the counts mid-sweep.
#   => (5+20) * 8 = 200 ops per size, 1000 ops, 3,174,400 B, all from rank 0.
#
#   -w allocate -s flush are the defaults (osu_put_bw.c:36) and are left alone.
BW_ARGS=(-m 512:8192 -W 8 -i 20 -x 5)
# One 4-byte MPI_INT operation per iteration, 25 of them, one window. MPI_INT
# because UCX's atomic doors take 4- and 8-byte operands and nothing else.
AT_ARGS=(-m 4:4 -T MPI_INT -i 20 -x 5)
# The knob: the SAME benchmark at its default one-byte MPI_CHAR.
CHAR_ARGS=(-m 4:4 -T MPI_CHAR -i 20 -x 5)
# ---------------------------------------------------------------------------

# The UCX tree belonging to the MPI THAT WILL ACTUALLY RUN. The provider
# interposes public UCP symbols; build against one libucp and run against
# another and the interposition silently does not engage.
UCX_HOME_RESOLVED="$(pcomm_find_ucx)" || {
  echo "!! no HPC-X UCX tree found beside this MPI ($(command -v mpirun))" >&2; exit 1; }
export LD_LIBRARY_PATH="$UCX_HOME_RESOLVED/lib:$LD_LIBRARY_PATH"

echo "=== the stack this job resolves ==="
"$UCX_HOME_RESOLVED/bin/ucx_info" -v 2>/dev/null | head -3
ldd "$OSU/osu_put_bw" | grep -E 'libmpi|libucp|libucs' || true
ompi_info --version 2>/dev/null | head -2
# The one-sided component actually available. Nothing in a capture records which
# OSC component ran (QUESTIONS.md #8), so it is printed here or it is unknowable
# afterwards.
ompi_info --param osc ucx --level 9 2>/dev/null | head -20

# --mca osc ucx is LOAD-BEARING. With osc/rdma or osc/pt2pt the same MPI_Put
# becomes an active message, no remote key is ever unpacked, and every table
# this study is about is empty for a reason that has nothing to do with pcomm.
# Set twice on purpose: as an mpirun flag, and in the environment, so that a
# launch that goes through srun rather than mpirun still gets it.
export OMPI_MCA_osc=ucx OMPI_MCA_pml=ucx
MPI_ARGS=(-np 2 --map-by ppr:1:node --mca osc ucx --mca pml ucx
          -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH -x OMPI_MCA_osc -x OMPI_MCA_pml)

###############################################################################
# PHASE 1 — THE BARE RUNS. Not optional, and the apps brief says why: a capture
# of a workload that did not work is indistinguishable from a tracer that
# captured nothing, and that confusion has cost this project several slots. Same
# arguments as the traced runs, so the bandwidth and latency lines below are the
# reference the traced runs are read against.
###############################################################################
echo "############ PHASE 1: BARE ############"
# Returns the WORKLOAD's exit status, not echo's. Getting that wrong is how a
# `|| exit 1` guard silently never fires and a broken bare run is reported as a
# clean one.
bare() {
  local name="$1"; shift
  echo "--- bare $name $*"
  mpirun "${MPI_ARGS[@]}" "$OSU/$name" "$@"
  local rc=$?
  echo "bare $name exit=$rc"
  return $rc
}

bare osu_put_bw       "${BW_ARGS[@]}"   || { echo "!! bare osu_put_bw failed — nothing below is worth capturing" >&2; exit 1; }
bare osu_get_bw       "${BW_ARGS[@]}"   || { echo "!! bare osu_get_bw failed" >&2; exit 1; }
bare osu_acc_latency  "${AT_ARGS[@]}"   || echo "!! bare osu_acc_latency failed — its capture will be read as such"
bare osu_fop_latency  "${AT_ARGS[@]}"   || echo "!! bare osu_fop_latency failed — its capture will be read as such"
# The knob arm may legitimately fail: a one-byte atomic is not something UCX can
# do, and whether OpenMPI falls back or propagates the error is exactly what
# this arm is here to find out. It must not take the job down with it.
bare osu_fop_latency  "${CHAR_ARGS[@]}" || echo "!! bare osu_fop_latency -T MPI_CHAR failed — that is a result, not a crash"

###############################################################################
# PHASE 2 — THE CAPTURES. -p ucx,ucx_deep: both are hook-based and therefore
# non-exclusive, so they co-run in ONE pass rather than forcing a replay, and
# the study gets the application's intent and the wire it became in one file.
###############################################################################
echo "############ PHASE 2: CAPTURED ############"
# The output directory is computed by the CALLER and passed in. Deriving it
# inside the function and recovering it from the function's stdout would mean
# piping the whole arm through `tail`, and everything pcomm printed on the way —
# which is the first thing to read when an arm goes wrong — would vanish with it.
#
# THE PRIMARY ARM IS NAMED FOR THE STUDY, not for the arm: the aggregator looks
# for `<case-without-its-NN_-prefix>-<jobid>`, and naming the primary output for
# its arm is a mistake that has cost this project two separate queue slots.
capture() {  # capture <outdir> <providers> <binary> <args...>
  local out="$1" prov="$2" bin="$3"; shift 3
  echo "======== capturing $bin -> $out  (-p $prov)"
  rm -rf "$out"
  mpirun "${MPI_ARGS[@]}" "$PCOMM" run -p "$prov" -o "$out" -- "$OSU/$bin" "$@"
  local rc=$?
  echo "arm=$(basename "$out") exit=$rc"
  ls -d "$out"/rank-* 2>/dev/null \
    || echo "!! no rank-* under $out — the workload never started under pcomm"
  "$PCOMM" read   "$out"
  "$PCOMM" export "$out" --csv "$out.csv"
  pcomm_derive    "$out"
  return $rc
}

OUT_PUT="$(pcomm_out osu_onesided)"
OUT_GET="$(pcomm_out osu_onesided_get)"
OUT_ACC="$(pcomm_out osu_onesided_acc)"
OUT_FOP="$(pcomm_out osu_onesided_fop)"
OUT_CHR="$(pcomm_out osu_onesided_char)"
OUT_IB="$(pcomm_out osu_onesided_ib)"

capture "$OUT_PUT" ucx,ucx_deep osu_put_bw      "${BW_ARGS[@]}"   || true
capture "$OUT_GET" ucx,ucx_deep osu_get_bw      "${BW_ARGS[@]}"   || true
capture "$OUT_ACC" ucx,ucx_deep osu_acc_latency "${AT_ARGS[@]}"   || true
capture "$OUT_FOP" ucx,ucx_deep osu_fop_latency "${AT_ARGS[@]}"   || true
# The knob arm and the shared-pass arm. Neither is judged by an assertion
# directory — the first is expected to have its operations refused and the
# second has a different provider list — so both are printed, and a non-zero
# exit from either is a result rather than a reason to stop.
capture "$OUT_CHR" ucx,ucx_deep osu_fop_latency "${CHAR_ARGS[@]}" || true
capture "$OUT_IB"  ucx,ib       osu_put_bw      "${BW_ARGS[@]}"   || true

###############################################################################
# PHASE 3 — WHAT A HUMAN SHOULD SEE WITHOUT RUNNING SQL. The door vocabulary is
# not published anywhere a black-box user may read (QUESTIONS.md #1), so no
# assertion in this study names a door. That makes printing them the only way
# the question "which door did MPI actually take" gets answered at all.
###############################################################################
echo "############ PHASE 3: THE DOORS, THE KEYS, THE KNOB ############"
doors() {
  local out="$1"; echo "======== $out"
  for t in ucx_rma ucx_atomic ucx_rkey ucx_rkey_pack ucx_mem_map ucx_tag_send uct_send ib_mem_region ib_post_send; do
    if [ -f "$out.csv/$t.csv" ]; then
      printf '%-16s %s rows\n' "$t" "$(($(wc -l < "$out.csv/$t.csv") - 1))"
    fi
  done
  # Header-driven column lookup: the CSV prepends `rank` to each table's own
  # column list, and hard-coded field numbers are how case 21's comment records
  # a reader going wrong.
  bycol() {  # bycol <csv> <colname>...
    local f="$1"; shift
    [ -f "$f" ] || return 0
    awk -F, -v want="$*" 'NR==1{n=split(want,w," ");for(i=1;i<=NF;i++)for(j=1;j<=n;j++)if($i==w[j])c[++m]=i;next}
                          {k="";for(i=1;i<=m;i++)k=k (i>1?"/":"") $c[i]; t[k]++}
                          END{for(x in t) printf "   %-28s %d\n", x, t[x]}' "$f" | sort
  }
  echo "-- ucx_rma  door/op/count";       bycol "$out.csv/ucx_rma.csv"    door op count
  echo "-- ucx_atomic op/op_size/status"; bycol "$out.csv/ucx_atomic.csv" op op_size status
  echo "-- uct_send op";                  bycol "$out.csv/uct_send.csv"   op
  if [ -d "$out.pq" ]; then
    "$PCOMM" sql "$out.pq" \
      "SELECT rank, tl_name, op, sum(ops) AS ops, sum(out_bytes) AS out_bytes,
              sum(in_bytes) AS in_bytes FROM transport_mix GROUP BY 1,2,3 ORDER BY 1,2,3" 2>&1
    "$PCOMM" sql "$out.pq" "SELECT finding, severity, n, note FROM link_health WHERE n > 0 ORDER BY severity" 2>&1
    "$PCOMM" sql "$out.pq" "SELECT * FROM comm_edges ORDER BY bytes DESC" 2>&1
  fi
}
for a in "$OUT_PUT" "$OUT_GET" "$OUT_ACC" "$OUT_FOP" "$OUT_CHR" "$OUT_IB"; do
  [ -n "$a" ] && doors "$a"
done

# THE KNOB, SIDE BY SIDE. One variable changed — the atomic operand width, 4
# bytes to 1 — and the claim is that the capture SHOWS what it did. Printed as a
# diff so the answer is a diff rather than a paragraph.
echo "=== the knob: MPI_INT (4 B) against MPI_CHAR (1 B) fetch-and-op ==="
for a in "$OUT_FOP" "$OUT_CHR"; do
  [ -f "$a.csv/ucx_atomic.csv" ] && {
    echo "--- $a"
    awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
             {t[$h["op"]"/"$h["op_size"]"/"$h["status"]]++}
             END{for(x in t) print "   " x, t[x]}' "$a.csv/ucx_atomic.csv" | sort
    echo "    ucx_rma rows: $(($(wc -l < "$a.csv/ucx_rma.csv" 2>/dev/null || echo 1) - 1))"
  }
done

###############################################################################
# PHASE 4 — THE ASSERTIONS. Empty result = pass. The two bandwidth arms share
# one oracle because their arguments are identical and no assertion names the
# direction; the two atomic arms share the other.
###############################################################################
echo "############ PHASE 4: ASSERTIONS ############"
for a in "$OUT_PUT" "$OUT_GET"; do
  [ -n "$a" ] && bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$a.csv" "$STUDY"
done
for a in "$OUT_ACC" "$OUT_FOP"; do
  [ -n "$a" ] && bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$a.csv" "$STUDY/arms/atomic"
done
