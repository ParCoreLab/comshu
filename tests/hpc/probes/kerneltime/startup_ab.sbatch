#!/bin/bash
# PROBE — does kernel timing cost 150 seconds of STARTUP at 64 ranks?
#
# The question comes from job 44552862, whose first ib_post_send landed 165.5 s
# after its first clock anchor. The same workload at the same scale on the same
# machine started in 8.2 s (44542856) and 12.9 s (44517390). Kernel timing is
# the obvious difference and it is NOT the only one: those runs used a different
# build and a different 16-node allocation, on a shared GPFS whose startup cost
# is exactly the kind of thing that varies between allocations.
#
# So this job changes ONE thing. Arms B and C are the same binary, the same
# build, the same allocation, the same workload — `-p ib` against `-p ib,cupti`.
# If C - B is ~150 s, kernel timing owns it. If C - B is seconds and A is also
# slow, the 165 s was the allocation and the plan's "+0.65%" still stands.
#
# Interleaved and repeated, because one sample per arm cannot separate a real
# effect from the variance we are trying to rule out.
#
#   sbatch tests/hpc/probes/kerneltime/startup_ab.sbatch
#
#SBATCH --job-name=pcomm-kt-startup-ab
#SBATCH --account=etur02
#SBATCH --qos=acc_ehpc
#SBATCH --nodes=16
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:50:00
#SBATCH --output=%x.%j.out

set -x
export PCOMM_ROOT="${PCOMM_ROOT:-$HOME/pcomm-impl}"
export PCOMM_BUILD="${PCOMM_BUILD:-$PCOMM_ROOT/build-cupti}"
export PCOMM_PROVIDER_PATH="$PCOMM_BUILD/gpu-backends/cupti"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
_pcomm_module load cuda/12.6
CUPTI_LIB="$CUDA_HOME/extras/CUPTI/lib64"
NSYS="$CUDA_HOME/bin/nsys"
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$CUPTI_LIB:$LD_LIBRARY_PATH"

APP="$HOME/asym_patterns-sm90"
[ -x "$APP" ] || { echo "!! no $APP — run the corpus job first" >&2; exit 1; }

# iters 3: this probe measures STARTUP, and a long data phase only adds noise
# to the wall clock the arms are compared on.
ARGS="hub 4096 3"
MPI_COMMON=(-np 64 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH
            -x NVSHMEM_BOOTSTRAP=MPI -x NVSHMEM_DISABLE_P2P=1)

run_arm() {
  local tag="$1"; shift
  echo "=== ARM $tag rep $REP ==="
  local t0=$(date +%s.%N)
  "$@"
  local rc=$? t1=$(date +%s.%N)
  echo "ARM_RESULT tag=$tag rep=$REP rc=$rc wall_s=$(echo "$t1 - $t0" | bc)"
}

for REP in 1 2 3; do
  # A — no pcomm at all. The floor: what this allocation costs to start.
  run_arm A_bare mpirun "${MPI_COMMON[@]}" "$APP" $ARGS

  # B — what we ship today.
  OB="$HOME/kt-ab-B-$SLURM_JOB_ID-$REP"; rm -rf "$OB"
  run_arm B_ib mpirun "${MPI_COMMON[@]}" -x PCOMM_PROVIDER_PATH \
    "$PCOMM" run -p ib -o "$OB" -- "$APP" $ARGS

  # C — the same build and the same allocation, one provider more.
  OC="$HOME/kt-ab-C-$SLURM_JOB_ID-$REP"; rm -rf "$OC"
  run_arm C_ib_cupti mpirun "${MPI_COMMON[@]}" -x PCOMM_PROVIDER_PATH -x PCOMM_VERBOSE=1 \
    "$PCOMM" run -p ib,cupti -o "$OC" -- "$APP" $ARGS
done

# D — where the time goes, if it goes anywhere. nsys on RANK 0 ONLY: 64 reports
# would cost more than they explain, and every rank is gated by rank 0 through
# NVSHMEM's barriers anyway. `--trace=cuda,mpi` is deliberately heavier than our
# backend — the point is to see WHICH phase is slow, not to measure our cost.
cat > "$HOME/nsys_rank0.sh" <<'EOS'
#!/bin/bash
if [ "${OMPI_COMM_WORLD_RANK:-0}" = "0" ]; then
  exec "$NSYS" profile -o "$HOME/kt-ab-nsys-$SLURM_JOB_ID" --trace=cuda,mpi \
       --mpi-impl=openmpi --force-overwrite=true "$@"
fi
exec "$@"
EOS
chmod +x "$HOME/nsys_rank0.sh"
OD="$HOME/kt-ab-D-$SLURM_JOB_ID"; rm -rf "$OD"
run_arm D_cupti_nsys mpirun "${MPI_COMMON[@]}" -x PCOMM_PROVIDER_PATH -x NSYS="$NSYS" \
  -x SLURM_JOB_ID -x HOME "$HOME/nsys_rank0.sh" \
  "$PCOMM" run -p ib,cupti -o "$OD" -- "$APP" $ARGS

# The capture-side measure of the same thing, and the one directly comparable to
# the 165.5 s that prompted this: first send minus first anchor, per arm.
echo "=== startup measured FROM THE CAPTURES (first send - first anchor) ==="
for d in "$HOME"/kt-ab-B-"$SLURM_JOB_ID"-* "$HOME"/kt-ab-C-"$SLURM_JOB_ID"-* "$OD"; do
  [ -d "$d" ] || continue
  echo "--- $(basename "$d")"
  "$PCOMM" read "$d" 2>&1 | tail -3
done

echo "=== nsys summaries (rank 0, arm D) ==="
R="$HOME/kt-ab-nsys-$SLURM_JOB_ID.nsys-rep"
if [ -f "$R" ]; then
  "$NSYS" stats --report mpi_event_sum --format table "$R" 2>/dev/null | head -14
  "$NSYS" stats --report cuda_api_sum --format table "$R" 2>/dev/null | head -18
else
  echo "no nsys report at $R"
fi
echo "=== DONE ==="
