#!/bin/bash
# 01_gromacs — GROMACS 2025.2 under comshu, and the GMX_DISABLE_DIRECT_GPU_COMM
# comparison, in ONE allocation.
#
# FOUR mdrun invocations, same binary, same tpr, same command line:
#
#   1. bare,  direct GPU comm ON     <- proves the workload works, and is the
#   2. bare,  direct GPU comm OFF       only place ns/day means anything
#   3. comshu, direct GPU comm ON      <- the capture pair whose difference IS
#   4. comshu, direct GPU comm OFF        the finding
#
# The bare pair is not optional (apps/README.md: "a capture of a workload that
# did not work is indistinguishable from a tracer that captured nothing").
#
#   sbatch tests/hpc/apps/01_gromacs/job.sbatch
#
#SBATCH --job-name=comshu-app-gromacs
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=01:45:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime                       # gcc 13.2 (GLIBCXX_3.4.32) + HPC SDK 25.7

# ---------------------------------------------------------------- GROMACS ----
# Load order matters: lmod evaluates gromacs's prereq() against what is loaded
# AT THAT MOMENT, so `module load nvidia-hpc-sdk fftw gromacs` on one line fails
# the prereq and rolls the whole transaction back — silently, leaving PATH
# unchanged and `which gmx_mpi` empty. One module per command.
module load fftw/3.3.10-gcc-nvhpcx
module load gromacs/2025.2-gcc-nvhpcx

# The MPI gmx_mpi is LINKED against is HPC-X 2.16 out of HPC SDK **23.11**
# (readelf: libmpi.so.40 -> .../23.11/.../hpcx-2.16/ompi/lib), not the 25.7 that
# comshu_load_runtime puts on PATH. Launching it with 25.7's mpirun mixes two
# Open MPI releases in one job. Worse for us: libucx_trace.so carries a RUNPATH
# to 25.7's hpcx-2.22.1 UCX, and because it is preloaded FIRST its libucp.so.0
# wins the SONAME — Open MPI 4.1.5's pml_ucx would then be running against UCX
# 1.18. Pinning both to 2.16 keeps the traced stack the stack the module ships.
# New dtags mean LD_LIBRARY_PATH beats the baked RUNPATH.
HPCX=/gpfs/apps/MN5/ACC/NVIDIA-HPC-SDK/23.11/Linux_x86_64/23.11/comm_libs/12.3/hpcx/hpcx-2.16
export PATH="$HPCX/ompi/bin:$PATH"
export LD_LIBRARY_PATH="$HPCX/ucx/lib:$HPCX/ompi/lib:$LD_LIBRARY_PATH"
export OPAL_PREFIX="$HPCX/ompi"
export SLURM_CPU_BIND=none                # the module banner asks for this

# The module sets GMX_ENABLE_DIRECT_GPU_COMM=1. That variable has not existed in
# GROMACS since 2022 — 2025.2 reads GMX_DISABLE_DIRECT_GPU_COMM and nothing else
# (decidegpuusage.cpp:794), direct comm being ON by default in a CUDA + library
# MPI build. Unset it so the job cannot be misread as having enabled anything.
unset GMX_ENABLE_DIRECT_GPU_COMM
unset GMX_DISABLE_DIRECT_GPU_COMM

echo "=== stack ==="
which gmx_mpi mpirun
ldd "$(which gmx_mpi)" | grep -E 'libmpi|libcuda|libcufftMp|libnvshmem'
# WHICH libucp the traced process will actually load. If this names hpcx-2.22.1
# the pinning above did not take and the ucx capture is against a UCX that
# Open MPI was not built for.
ldd "$COMSHU_BUILD/providers/ucx/libucx_trace.so" | grep -E 'libucp|not found'

WORK="$HOME/comshu-apps/gromacs"
TPR="$WORK/benchRIB.tpr"                  # 2,136,412 atoms; md5 f16f0e02…
RUN="$WORK/run-$SLURM_JOB_ID"; mkdir -p "$RUN"; cd "$RUN" || exit 1

# ------------------------------------------------------------ the command ----
# Pinned for comparability, not for speed. Each of these is here because leaving
# it to `auto` makes the two arms different experiments:
#   -npme 1        7 PP + 1 PME. 7 is prime, so -dd admits only 7x1x1 and the
#                  halo partner set is forced to (x±1) mod 7 with no tuning.
#   -ddorder pp_pme  PME is rank 7. (interleave lands on the same answer at
#                  npme=1, but stating it removes a default from the oracle.)
#   -notunepme     PME tuning shifts the cutoff/grid split over the first few
#                  hundred steps, changing every message size it touches.
#   -dlb no        DLB resizes DD cells, so halo sizes would drift within a run
#                  and between the arms.
#   -nstlist 100   fixes the pair-list / DD-rebuild interval; otherwise GROMACS
#                  retunes it from measured performance, and the tracer changes
#                  the measurement.
#   -update auto   NOT `gpu`: with direct comm disabled, GPU update under DD is
#                  unsupported and an explicit -update gpu is FATAL
#                  (decidegpuusage.cpp, updateTarget == Gpu -> GMX_THROW).
#                  `auto` is what a real user gets, and the fallback to CPU
#                  update is part of what the knob does.
COMMON_ARGS=(-s "$TPR" -npme 1 -dd 7 1 1 -ddorder pp_pme
             -nb gpu -pme gpu -notunepme -dlb no -nstlist 100
             -ntomp "${SLURM_CPUS_PER_TASK:-20}" -pin on -noconfout)
NSTEPS=1000

# `mpirun -x VAR` on an unset VAR is a warning, and exporting the knob as an
# EMPTY string would disable direct comm — getenv() returns non-NULL for "".
# So the -x list is built per run instead of being a constant.
mpi_run() {                               # $@ = the command to launch
  local xs=(-x PATH -x LD_LIBRARY_PATH -x OPAL_PREFIX -x COMSHU_PROVIDER_PATH)
  [ -n "${GMX_DISABLE_DIRECT_GPU_COMM:-}" ] && xs+=(-x GMX_DISABLE_DIRECT_GPU_COMM)
  [ -n "${GMX_FORCE_GPU_AWARE_MPI:-}"    ] && xs+=(-x GMX_FORCE_GPU_AWARE_MPI)
  mpirun -np 8 --map-by ppr:4:node --bind-to none "${xs[@]}" "$@"
}

arm() {                                   # $1 = on|off — set the one variable
  if [ "$1" = off ]; then export GMX_DISABLE_DIRECT_GPU_COMM=1
  else                    unset  GMX_DISABLE_DIRECT_GPU_COMM; fi
}

# A 120 MB tpr off GPFS plus a 2.1 M-atom DD setup costs more than a minute
# before step 0, so an unreadable tpr (benchRIB is tpx v83, GROMACS 4.6 era) or
# a rejected -dd would otherwise only surface after three long runs.
echo "=== smoke: nsteps 0 ==="
arm on
mpi_run gmx_mpi mdrun "${COMMON_ARGS[@]}" -nsteps 0 \
        -g smoke.log -e smoke.edr -o smoke.trr -c smoke.gro \
  || { echo "!! SMOKE FAILED — tpr unreadable or -dd rejected; nothing below can run"
       tail -80 smoke.log 2>/dev/null; exit 1; }
grep -E 'atoms|Domain decomposition grid|PME ranks|separate PME|Using [0-9]+ MPI' smoke.log | head -20

run_bare() {                              # $1 = on|off
  local a="$1"; arm "$a"
  echo "=== bare $a (GMX_DISABLE_DIRECT_GPU_COMM=${GMX_DISABLE_DIRECT_GPU_COMM:-<unset>}) ==="
  mpi_run gmx_mpi mdrun "${COMMON_ARGS[@]}" -nsteps "$NSTEPS" \
          -g "bare-$a.log" -e "bare-$a.edr" -o "bare-$a.trr"
  echo "bare $a exit=$?"
  grep -E 'GPU-aware MPI|direct GPU communication|GPU halo exchange|Update task|ns/day|Performance:' \
       "bare-$a.log" | head -20
}

run_traced() {                            # $1 = on|off
  local a="$1" out; arm "$a"
  out="$WORK/gromacs-$a-$SLURM_JOB_ID"; rm -rf "$out" "$out.csv" "$out.pq"; mkdir -p "$out"
  echo "=== comshu $a (GMX_DISABLE_DIRECT_GPU_COMM=${GMX_DISABLE_DIRECT_GPU_COMM:-<unset>}) ==="
  mpi_run "$COMSHU" run -p ib,ucx -o "$out" -- \
          gmx_mpi mdrun "${COMMON_ARGS[@]}" -nsteps "$NSTEPS" \
          -g "trace-$a.log" -e "trace-$a.edr" -o "trace-$a.trr"
  echo "comshu $a exit=$?"
  echo "--- captured streams ($a) ---"; "$COMSHU" read "$out"
  "$COMSHU" export "$out" --csv "$out.csv"
  comshu_derive "$out"
  grep -E 'GPU-aware MPI|direct GPU communication|GPU halo exchange|Update task|Performance:' \
       "trace-$a.log" | head -20
}

run_bare on
# GROMACS only takes the direct path when it believes the MPI library is
# GPU-aware. If detection failed there is no experiment — both arms would be the
# staged path — so force it and repeat. Reading the bare log to decide is
# exactly why the bare runs come first.
if ! grep -q 'GPU-aware MPI detected' bare-on.log; then
  echo "!! GPU-aware MPI NOT auto-detected — forcing GMX_FORCE_GPU_AWARE_MPI=1"
  export GMX_FORCE_GPU_AWARE_MPI=1
  run_bare on
fi
run_bare off

run_traced on
run_traced off

# ------------------------------------------------------------- assertions ----
# NOT suite/sql_check.sh — its `params` column list is hard-coded and rejects
# npp/npme/nsteps/natoms, which is this case's entire oracle. See meta.json's
# runner_note and NOTES.md.
CASE="$COMSHU_ROOT/tests/hpc/apps/01_gromacs"
for a in on off; do
  echo "=== asserts: direct_$a ==="
  bash "$CASE/asserts.sh" "$WORK/gromacs-$a-$SLURM_JOB_ID.csv" "$CASE" "direct_$a"
done

echo "=== artefacts ==="
ls -la "$RUN"
du -sh "$WORK"/gromacs-*-"$SLURM_JOB_ID"* 2>/dev/null
