#!/bin/bash
# 01_gromacs ROUND 2 — GROMACS 2025.2 under `-p ucx,ucx_deep`, to ask whether
# transport_mix distinguishes the shared-memory half of a real domain
# decomposition from the fabric half.
#
# The oracle is PREDICTION.md, committed before this script had ever run.
#
# FIVE mdrun invocations in ONE allocation, in this order and for these reasons:
#
#   1. bare,  direct GPU comm ON   \  the workload works, and the ONLY place
#   2. bare,  direct GPU comm OFF  /  ns/day and the DD log lines mean anything
#   3. comshu, direct GPU comm ON      the primary capture
#   4. comshu, direct GPU comm OFF     the knob's control arm
#   5. comshu, ON + UCX tag offload    the tag_flow control, 50 steps
#
# The bare pair is not optional (apps/README.md: "a capture of a workload that
# did not work is indistinguishable from a tracer that captured nothing"). Run 1
# is also where the two oracle lines are read: the DD grid and the number of
# communication pulses. If the pulse count is not 1, PREDICTION.md P1.2 is void
# and the halo partner set is larger than (x+-1) mod 7 — the script says so
# loudly rather than letting the assertions fail mysteriously.
#
#   sbatch tests/hpc/apps/01_gromacs/job-r2.sbatch
#
# ONE JOB AT A TIME, ACCOUNT-WIDE. Before submitting:
#   while squeue -u koc296797 -h | grep -q .; do sleep 60; done
#
#SBATCH --job-name=comshu-app-gromacs-r2
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=01:00:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime                       # gcc 13.2 (GLIBCXX_3.4.32) + HPC SDK 25.7

# ---------------------------------------------------------------- GROMACS ----
# One module per command: lmod evaluates gromacs's prereq() against what is
# loaded AT THAT MOMENT, and a combined load fails the prereq and rolls the
# whole transaction back SILENTLY. See NOTES.md.
module load fftw/3.3.10-gcc-nvhpcx
module load gromacs/2025.2-gcc-nvhpcx

# gmx_mpi is linked against HPC-X 2.16 out of HPC SDK 23.11, not the 25.7 that
# comshu_load_runtime puts on PATH. This matters more for round 2 than it did for
# round 1: ucx_deep patches uct_iface_ops_t, an installed header with no ABI
# promise, and it must patch the SAME UCX the application's pml_ucx is running
# against. If libucx_deep_trace.so resolves libucp/libuct from hpcx-2.22.1 while
# Open MPI 4.1.5 runs against 1.15, the layout guard is comparing the wrong two
# things. New dtags mean LD_LIBRARY_PATH beats the baked RUNPATH.
HPCX=/gpfs/apps/MN5/ACC/NVIDIA-HPC-SDK/23.11/Linux_x86_64/23.11/comm_libs/12.3/hpcx/hpcx-2.16
export PATH="$HPCX/ompi/bin:$PATH"
export LD_LIBRARY_PATH="$HPCX/ucx/lib:$HPCX/ompi/lib:$LD_LIBRARY_PATH"
export OPAL_PREFIX="$HPCX/ompi"
export SLURM_CPU_BIND=none                # the module banner asks for this

# The module sets GMX_ENABLE_DIRECT_GPU_COMM=1, a variable GROMACS dropped in
# 2022. Unset it so nothing can be misread as having enabled anything. Never
# BLANK the disable variable — getenv() returns non-NULL for "" and that turns
# direct comm OFF.
unset GMX_ENABLE_DIRECT_GPU_COMM
unset GMX_DISABLE_DIRECT_GPU_COMM
unset COMSHU_UCX_DEEP_FORCE                # a forced binding is not an ordinary pass

echo "=== stack ==="
which gmx_mpi mpirun
ldd "$(which gmx_mpi)" | grep -E 'libmpi|libcuda|libcufft'
# WHICH UCX each provider will load. Both must name hpcx-2.16. If either names
# 2.22.1 the pinning did not take: stop and fix it rather than spending the run,
# because uct_binding would then be guarding the wrong library.
for so in providers/ucx/libucx_trace.so providers/ucx_deep/libucx_deep_trace.so; do
  echo "--- $so"
  ldd "$COMSHU_BUILD/$so" 2>&1 | grep -E 'libucp|libuct|libucs|not found'
done

WORK="$HOME/comshu-apps/gromacs"
TPR="$WORK/benchRIB.tpr"                  # 2,136,412 atoms; md5 f16f0e02...
RUN="$WORK/r2-$SLURM_JOB_ID"; mkdir -p "$RUN"; cd "$RUN" || exit 1
CASE="$COMSHU_ROOT/tests/hpc/apps/01_gromacs"

# ------------------------------------------------------------ the command ----
# Identical to round 1's, so the two rounds are the same experiment, EXCEPT
# nsteps. See meta-r2.json: ucx_deep records one row per WIRE operation and an
# intra-node rendezvous can fragment, so 1000 steps risks a capture two orders
# of magnitude larger than round 1's 55 MB. Every oracle in this round is a
# ratio or a set, so the step count enters none of them.
COMMON_ARGS=(-s "$TPR" -npme 1 -dd 7 1 1 -ddorder pp_pme
             -nb gpu -pme gpu -notunepme -dlb no -nstlist 100
             -ntomp "${SLURM_CPUS_PER_TASK:-20}" -pin on -noconfout)

# THE STEP COUNTS COME OUT OF meta-r2.json AND ARE NOT WRITTEN HERE. They used
# to be `NSTEPS=200` and `NSTEPS_TM=50` literals beside a meta file that said the
# same thing, which is two copies of one number, and on job 45235734 the copies
# disagreed with a THIRD one: the round-1 assertions at the bottom of this script
# were evaluated against meta.json's nsteps=1000 and reported the capture at
# exactly 20 % of expected. The capture was correct; the oracle was a step count
# from a different run. Read the number once, from the file the assertions read.
meta_param() {  # $1 = key, $2 = variant ("" for the base workload)
  python3 - "$CASE/meta-r2.json" "$1" "${2:-}" <<'PY'
import json, sys
w = dict(json.load(open(sys.argv[1]))["workload"])
if sys.argv[3]:
    w.update(json.load(open(sys.argv[1]))["variants"][sys.argv[3]])
print(w[sys.argv[2]])
PY
}
NSTEPS="$(meta_param nsteps)"       || { echo "!! cannot read nsteps from meta-r2.json"; exit 1; }
NSTEPS_TM="$(meta_param nsteps tm)" || { echo "!! cannot read variants.tm.nsteps from meta-r2.json"; exit 1; }
case "$NSTEPS$NSTEPS_TM" in *[!0-9]*|"") echo "!! bad nsteps from meta-r2.json: '$NSTEPS' '$NSTEPS_TM'"; exit 1;; esac
echo "nsteps=$NSTEPS  nsteps_tm=$NSTEPS_TM  (from meta-r2.json, the same file the assertions read)"

mpi_run() {
  local xs=(-x PATH -x LD_LIBRARY_PATH -x OPAL_PREFIX -x COMSHU_PROVIDER_PATH)
  [ -n "${GMX_DISABLE_DIRECT_GPU_COMM:-}" ] && xs+=(-x GMX_DISABLE_DIRECT_GPU_COMM)
  [ -n "${GMX_FORCE_GPU_AWARE_MPI:-}"    ] && xs+=(-x GMX_FORCE_GPU_AWARE_MPI)
  [ -n "${UCX_RC_MLX5_TM_ENABLE:-}"      ] && xs+=(-x UCX_RC_MLX5_TM_ENABLE)
  [ -n "${UCX_DC_MLX5_TM_ENABLE:-}"      ] && xs+=(-x UCX_DC_MLX5_TM_ENABLE)
  [ -n "${UCX_TM_THRESH:-}"              ] && xs+=(-x UCX_TM_THRESH)
  mpirun -np 8 --map-by ppr:4:node --bind-to none "${xs[@]}" "$@"
}

arm() {
  if [ "$1" = off ]; then export GMX_DISABLE_DIRECT_GPU_COMM=1
  else                    unset  GMX_DISABLE_DIRECT_GPU_COMM; fi
}

echo "=== smoke: nsteps 0 ==="
arm on
mpi_run gmx_mpi mdrun "${COMMON_ARGS[@]}" -nsteps 0 \
        -g smoke.log -e smoke.edr -o smoke.trr -c smoke.gro \
  || { echo "!! SMOKE FAILED — nothing below can run"; tail -80 smoke.log; exit 1; }

# ------------------------------------------------------------- the oracles ---
# THE TWO LINES THE WHOLE STUDY RESTS ON. Printed from GROMACS's own log, not
# from a capture, and printed BEFORE any capture exists.
echo "=== DD ORACLE (from GROMACS, not from comshu) ==="
grep -E 'Domain decomposition grid|separate PME ranks|number of communication pulses|domain decomposition cell size|Using .* MPI process' smoke.log
PULSES="$(sed -n 's/.*initial number of communication pulses is:[[:space:]]*X[[:space:]]*\([0-9]*\).*/\1/p' smoke.log | head -1)"
echo "pulses_x = ${PULSES:-<not printed>}  (PREDICTION.md P1.2 predicts 1)"
if [ "${PULSES:-1}" != 1 ]; then
  echo "!! PULSES != 1 — the halo partner set is NOT (x+-1) mod 7 and every edge"
  echo "!! count in PREDICTION.md P1.3/P2.1 is VOID. The capture is still worth"
  echo "!! taking; the assertions that use npp are not. Say so in the report."
fi

# What UCX will actually choose, read from a COMPUTE node (it does not answer
# usefully on a login node). These four decide whether the shared-memory half of
# the prediction can exist at all: if UCX_TLS excludes shm, GROMACS's intra-node
# traffic goes over the NIC by loopback and the 2/7..4/7 bound does not apply.
echo "=== UCX configuration as this job will see it ==="
ucx_info -c 2>/dev/null | grep -E '^UCX_(TLS|RNDV_THRESH|ZCOPY_THRESH|TM_THRESH|RC_MLX5_TM_ENABLE|DC_MLX5_TM_ENABLE|IB_GPU_DIRECT_RDMA|IB_REG_METHODS)=' || echo "(ucx_info not on PATH)"
ucx_info -d 2>/dev/null | grep -E '^#[[:space:]]*(Memory|Transport|Device):' | head -40 || true

# ------------------------------------------------------------------ runs -----
run_bare() {
  local a="$1"; arm "$a"
  echo "=== bare $a (GMX_DISABLE_DIRECT_GPU_COMM=${GMX_DISABLE_DIRECT_GPU_COMM:-<unset>}) ==="
  mpi_run gmx_mpi mdrun "${COMMON_ARGS[@]}" -nsteps "$NSTEPS" \
          -g "bare-$a.log" -e "bare-$a.edr" -o "bare-$a.trr"
  echo "bare $a exit=$?"
  grep -E 'GPU-aware MPI|direct GPU communication|GPU halo exchange|Update task|Performance:|atoms communicated' \
       "bare-$a.log" | head -20
}

run_traced() {                            # $1 = on|off  $2 = tag  $3 = nsteps
  local a="$1" tag="$2" n="$3" out; arm "$a"
  out="$WORK/gmx-r2-$tag-$SLURM_JOB_ID"; rm -rf "$out" "$out.csv" "$out.pq"; mkdir -p "$out"
  echo "=== comshu $tag (direct=$a, nsteps=$n) ==="
  mpi_run "$COMSHU" run -p ucx,ucx_deep -o "$out" -- \
          gmx_mpi mdrun "${COMMON_ARGS[@]}" -nsteps "$n" \
          -g "trace-$tag.log" -e "trace-$tag.edr" -o "trace-$tag.trr"
  echo "comshu $tag exit=$?"

  # A capture directory with no rank-* subdirectory means the workload never
  # started. Read this before reading anything else.
  ls -d "$out"/rank-* >/dev/null 2>&1 || { echo "!! NO rank-* IN $out — the workload never started"; return 1; }

  echo "--- captured streams ($tag) ---"; "$COMSHU" read "$out"
  # uct_binding first, always: accepted=0 is why uct_send would be empty and the
  # record exists to say so.
  "$COMSHU" dump "$out" 806 2>/dev/null | head -12 || true
  "$COMSHU" export "$out" --csv "$out.csv"
  comshu_derive "$out"                     # parquet + the derived layer (DuckDB)
  grep -E 'GPU-aware MPI|direct GPU communication|GPU halo exchange|Update task|Performance:' \
       "trace-$tag.log" | head -20
}

run_bare on
# GROMACS takes the direct path only if it believes MPI is GPU-aware. If
# detection failed there is no experiment — both arms would be the staged path.
if ! grep -q 'GPU-aware MPI detected' bare-on.log; then
  echo "!! GPU-aware MPI NOT auto-detected — forcing GMX_FORCE_GPU_AWARE_MPI=1"
  export GMX_FORCE_GPU_AWARE_MPI=1
  run_bare on
fi
run_bare off

run_traced on  direct_on  "$NSTEPS"
run_traced off direct_off "$NSTEPS"

# The tag_flow control. MN5's HPC-X leaves ConnectX tag matching off, so
# uct_send.tag is never written and tag_flow is predicted to hold no matched
# flow in the two arms above. This arm asks the doors to fire, exactly as case
# 21's job script does, so that "the join is wrong" and "the fabric was not
# asked" are distinguishable. Short: it is a door-coverage question, not a
# volume one.
export UCX_RC_MLX5_TM_ENABLE=y
export UCX_DC_MLX5_TM_ENABLE=y
export UCX_TM_THRESH=1024
run_traced on tm "$NSTEPS_TM"
unset UCX_RC_MLX5_TM_ENABLE UCX_DC_MLX5_TM_ENABLE UCX_TM_THRESH

# ------------------------------------------------------------- assertions ----
# Over the PARQUET export, because transport_mix / tag_flow / tag_recv_resolved
# / link_health are built by `comshu link` and do not exist in the CSV one. See
# run-asserts-r2.sh's head comment.
for v in direct_on direct_off tm; do
  echo "=== asserts-r2: $v ==="
  bash "$CASE/run-asserts-r2.sh" "$WORK/gmx-r2-$v-$SLURM_JOB_ID.pq" "$CASE" "$v"
done

# Round 1's assertions, unchanged, over the CSV export of the primary arm. They
# are the raw-record checks and they still hold; running them is how a round-2
# capture is shown not to have regressed round 1's result.
#
# ASSERT_META=meta-r2.json, and it is not optional. The QUERIES are round 1's;
# the RUN is round 2's, and pme_coord_volume / pme_rank_force_volume are absolute
# volumes in nsteps. Judged against meta.json they are judged at 1000 steps
# against a 200-step run and report the capture at 20 % of expected — which is
# what happened on job 45235734. It also stops round 2 being judged against round
# 1's expect_tables, which lists the ib_* tables that `-p ucx,ucx_deep` cannot
# populate because the ib provider is not loaded.
echo "=== asserts (round 1's queries, round 2's params, CSV/sqlite) on the direct_on arm ==="
ASSERT_META=meta-r2.json \
  bash "$CASE/asserts.sh" "$WORK/gmx-r2-direct_on-$SLURM_JOB_ID.csv" "$CASE" "direct_on" || true

# ------------------------------------------------- the report's raw numbers ---
# Printed so the report can quote them without a second job. Every one of these
# is a query a reader can re-run.
PQ="$WORK/gmx-r2-direct_on-$SLURM_JOB_ID.pq"
echo "=== transport_mix, direct_on ==="
"$COMSHU" sql "$PQ" "SELECT rank, tl_name, dev_name, scope, op, ops, out_bytes, in_bytes, advertised_bytes, header_bytes FROM transport_mix ORDER BY rank, tl_name, op"
echo "=== per-rank network share (PREDICTION.md P2.3 predicts 80,60,60,80,20,0,20,57) ==="
"$COMSHU" sql "$PQ" "SELECT rank, sum(CASE WHEN scope='network' THEN out_bytes+in_bytes ELSE 0 END) AS network, sum(CASE WHEN scope='intra-node' THEN out_bytes+in_bytes ELSE 0 END) AS intra, sum(CASE WHEN scope='unknown' THEN out_bytes+in_bytes ELSE 0 END) AS unknown FROM transport_mix GROUP BY rank ORDER BY rank"
echo "=== the tl_name -> scope mapping this run actually produced ==="
"$COMSHU" sql "$PQ" "SELECT scope, tl_name, dev_name, count(*) AS rows, sum(ops) AS ops FROM transport_mix GROUP BY 1,2,3 ORDER BY scope, tl_name"
echo "=== the peer matrix, decoded from Open MPI's tag (round 1 said this was impossible) ==="
"$COMSHU" sql "$PQ" "SELECT rank, (tag >> 20) & 1048575 AS peer, tag >> 40 AS mpi_tag, count(*) AS n, sum(count) AS offered FROM ucx_tag_recv WHERE ((tag >> 20) & 1048575) < 8 GROUP BY 1,2,3 ORDER BY rank, offered DESC"
echo "=== MPI tags this run used (7 = eCommType_COORD, 8 = COORD_GPU, 0/1 = halo X/F) ==="
for v in direct_on direct_off; do
  echo "--- $v"
  "$COMSHU" sql "$WORK/gmx-r2-$v-$SLURM_JOB_ID.pq" \
    "SELECT tag >> 40 AS mpi_tag, count(*) AS sends, sum(count) AS units FROM ucx_tag_send GROUP BY 1 ORDER BY units DESC"
done
echo "=== tag_flow status distribution, all three arms ==="
for v in direct_on direct_off tm; do
  echo "--- $v"
  "$COMSHU" sql "$WORK/gmx-r2-$v-$SLURM_JOB_ID.pq" \
    "SELECT status, count(*) AS flows, sum(ucp_sends) AS ucp_sends, sum(uct_ops) AS uct_ops FROM tag_flow GROUP BY status" || true
done
echo "=== link_health rubric ==="
"$COMSHU" sql "$PQ" "SELECT finding, severity, n, note FROM link_health ORDER BY severity, n DESC"

echo "=== artefacts ==="
ls -la "$RUN"
du -sb "$WORK"/gmx-r2-*-"$SLURM_JOB_ID"* 2>/dev/null   # -sb, NOT -sh: GPFS block
                                                       # accounting lies about a
                                                       # directory just written
