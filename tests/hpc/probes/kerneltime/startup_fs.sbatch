#!/bin/bash
# PROBE — what owns the capture's 17x startup variance: the FILESYSTEM, or the
# VOLUME written to it?
#
# Job 44553961 measured, at 64 ranks, one binary, one allocation:
#
#   A bare        15.4  14.6  14.2   s   (+-4 %)
#   B -p ib      237.7  77.8  46.5   s
#   C -p ib,cupti 78.9 118.9  14.2   s
#
# So kernel timing is NOT the cost — C's best run equals bare and beat every B
# run — and the machine is not erratic, because bare is stable across the same
# allocation. Only the arms that WRITE A CAPTURE swing. A slow run is not
# uniformly slow either: in B rep 1 the first rank started sending at 77.6 s and
# the last at 146.8 s, a 69 s straggler spread, against 7.3-7.5 s in a fast run.
#
# Two candidate causes, and now two knobs, so this is a 2x2 rather than a guess:
#
#                     GPFS ($HOME)        node-local (/dev/shm)
#   --mode full       B_full_gpfs         B_full_shm
#   --mode simple     B_simple_gpfs       B_simple_shm
#
#   only the shm column is fast   -> the filesystem owns it
#   only the simple row is fast   -> the write VOLUME owns it, and `--mode
#                                    simple` (now the default) already fixes it
#   both matter                   -> they compound, which is the likeliest
#                                    outcome if it is I/O at all
#   neither                       -> not I/O; stop guessing and strace a
#                                    straggler rank
#
# --mode IS PASSED EXPLICITLY ON EVERY ARM, and that is not decoration. The
# default changed from full to simple while the previous version of this job sat
# in the queue, which would have silently rewritten what arms B and C recorded
# between submission and execution. An experiment must not read a default that
# can move under it.
#
#   sbatch tests/hpc/probes/kerneltime/startup_fs.sbatch
#
#SBATCH --job-name=comshu-kt-startup-fs
#SBATCH --account=etur02
#SBATCH --qos=acc_ehpc
#SBATCH --nodes=16
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=01:10:00
#SBATCH --output=%x.%j.out

set -x
export COMSHU_ROOT="${COMSHU_ROOT:-$HOME/comshu-impl}"
export COMSHU_BUILD="${COMSHU_BUILD:-$COMSHU_ROOT/build-cupti}"
export COMSHU_PROVIDER_PATH="$COMSHU_BUILD/gpu-backends/cupti"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime
_comshu_module load cuda/12.6
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$CUDA_HOME/extras/CUPTI/lib64:$LD_LIBRARY_PATH"

APP="$HOME/asym_patterns-sm90"
[ -x "$APP" ] || { echo "!! no $APP" >&2; exit 1; }
ARGS="hub 4096 3"
MPI_COMMON=(-np 64 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH
            -x NVSHMEM_BOOTSTRAP=MPI -x NVSHMEM_DISABLE_P2P=1 -x COMSHU_PROVIDER_PATH)

# The binaries this job actually ran, named in its own log: the whole reason
# the previous attempt was thrown away.
echo "=== build under test ==="; ls -la "$COMSHU_BUILD/providers/ibverbs/libibverbs_trace.so" \
     "$COMSHU_BUILD/libcomshu/libcomshu_engine.so" "$COMSHU"
"$COMSHU" run --help 2>&1 | grep -A1 -- "--mode" | head -3

# EVERY ARM IS BOUNDED. Job 44557384 lost an ORTE daemon on its FIRST arm --
# a node fault, nothing to do with this code -- and mpirun then hung rather
# than exiting, eating all 70 minutes of a 16-node allocation and producing
# zero measurements. An unbounded mpirun inside a loop of fifteen means any one
# infrastructure hiccup costs the whole job.
#
# 360 s is ~1.5x the slowest arm ever measured here (237.7 s). A timeout is
# reported as rc=124 and the loop CONTINUES: one dead arm should cost one arm,
# and the surviving reps still carry the comparison.
ARM_TIMEOUT="${ARM_TIMEOUT:-360}"

run_arm() {
  local tag="$1"; shift
  echo "=== ARM $tag rep $REP ==="
  local t0=$(date +%s.%N)
  timeout --kill-after=30 "$ARM_TIMEOUT" "$@"
  local rc=$? t1=$(date +%s.%N)
  [ "$rc" = "124" ] && echo "!! ARM $tag rep $REP TIMED OUT after ${ARM_TIMEOUT}s — continuing"
  echo "ARM_RESULT tag=$tag rep=$REP rc=$rc wall_s=$(echo "$t1 - $t0" | bc)"
}

shm_clean() { srun --ntasks-per-node=1 --ntasks=16 bash -c 'rm -rf /dev/shm/comshu-kt' >/dev/null 2>&1; }

for REP in 1 2 3; do
  run_arm A_bare mpirun "${MPI_COMMON[@]}" "$APP" $ARGS

  for MODE in full simple; do
    OG="$HOME/fs-$MODE-gpfs-$SLURM_JOB_ID-$REP"; rm -rf "$OG"
    run_arm B_${MODE}_gpfs mpirun "${MPI_COMMON[@]}" \
      "$COMSHU" run -p ib --mode "$MODE" -o "$OG" -- "$APP" $ARGS

    shm_clean
    run_arm B_${MODE}_shm mpirun "${MPI_COMMON[@]}" \
      "$COMSHU" run -p ib --mode "$MODE" -o /dev/shm/comshu-kt/$MODE-$REP -- "$APP" $ARGS
  done
done

# Stage one shm capture back so its per-rank straggler spread is computable too,
# and so the mode gate can be confirmed on a real 64-rank capture rather than
# only on the login-node pingpong it was verified against.
echo "=== staging the last /dev/shm capture ==="
srun --ntasks-per-node=1 --ntasks=16 bash -c \
  "mkdir -p $HOME/fs-simple-shm-$SLURM_JOB_ID && cp -r /dev/shm/comshu-kt/simple-3/. $HOME/fs-simple-shm-$SLURM_JOB_ID/ 2>/dev/null" || true
shm_clean

# 112 (ib_poll_cq) and 124 (ib_recv_cqe) must be ABSENT from every simple
# capture and PRESENT in every full one. If that inverts, the arms are mislabelled
# and no timing above means anything.
echo "=== mode gate, on the real captures ==="
for d in "$HOME/fs-full-gpfs-$SLURM_JOB_ID-3" "$HOME/fs-simple-gpfs-$SLURM_JOB_ID-3" \
         "$HOME/fs-simple-shm-$SLURM_JOB_ID"; do
  [ -d "$d" ] || continue
  echo "$(basename "$d"): streams=$(ls "$d"/rank-0/ 2>/dev/null | grep -oE '^[0-9]+' | sort -n | tr '\n' ' ')  bytes=$(du -sb "$d" | cut -f1)"
done
echo "=== DONE ==="
