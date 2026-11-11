#!/bin/bash
# Case 19 — ucx_mpi: MPI point-to-point over MN5's real UCX stack, captured by
# the public-UCP provider.
#
# WHY THIS CASE EXISTS. The UCX provider shipped with 29 interposed doors and 13
# record types, a byte-level schema conformance test, and a local gate that runs
# in 0.08 s — and it had NEVER produced a row on a cluster. Every `ucx_*` table
# in every capture in the corpus was empty, because no suite case loaded the
# provider and `run_on_mn5.sh` did not even build it. Stage 4 of
# plans/PLAN-20260815-1540 names exactly this: "a local gate exists ... there is
# no MN5 gate".
#
# Promoted from tests/hpc/probes/ucx_api_coverage/provider.sbatch, which
# established that this shape works (job 44675611). A probe answers a question
# once; a case keeps answering it, which is the difference that matters after
# 142 commits of drift.
#
#   sbatch tests/hpc/cases/19_ucx_mpi/job.sbatch
#
#SBATCH --job-name=comshu-19-ucx_mpi
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

# TWO NODES, ONE RANK EACH, ON PURPOSE. UCX picks shared memory for same-node
# pairs, and a shm-only run exercises the doors without ever touching the
# network — which would make this case green while saying nothing about the
# transport it exists to observe. Crossing a node boundary forces the rc/dc
# path.
MPI_BIN="$(dirname "$(command -v mpirun)")"
APP="$HOME/ucx_mpi_calls"
SRC="$COMSHU_ROOT/tests/hpc/probes/ucx_api_coverage/mpi_calls.c"

# Plain C and plain mpicc: this workload is deliberately NOT CUDA. The question
# is whether UCP doors are seen on MN5's MPI, and a GPU in the picture only adds
# a second provider's failure modes to a capture that is not about them.
if [ ! -x "$APP" ] || [ "$SRC" -nt "$APP" ]; then
  "$MPI_BIN/mpicc" -O2 -Wall -Wextra -Werror -o "$APP" "$SRC" \
    || { echo "!! mpicc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# UCX lives in the VERSIONED HPC-X tree, not under the comm_libs/mpi symlink that
# PATH resolves — and it must be the tree belonging to the MPI that will actually
# run, which is NOT $COMSHU_SDK. comshu_find_ucx is the one definition of that,
# shared with the build in run_on_mn5.sh: the provider interposes public UCP
# symbols, so building against one libucp and running against another leaves the
# interposition silently disengaged.
UCX_HOME_RESOLVED="$(comshu_find_ucx)" || {
  echo "!! no HPC-X UCX tree found beside this MPI ($(command -v mpirun))" >&2
  exit 1; }
UCX_LIB="$UCX_HOME_RESOLVED/lib"
if [ ! -e "$UCX_LIB/libucp.so" ]; then
  echo "!! no libucp.so under $UCX_LIB" >&2
  exit 1
fi
export LD_LIBRARY_PATH="$UCX_LIB:$LD_LIBRARY_PATH"

# Say which UCX is actually in play, every run. The provider interposes public
# UCP symbols, so a second libucp on the path is not a warning, it is a capture
# that silently records nothing.
echo "=== ucx this run resolves ==="
ls -la "$UCX_LIB/libucp.so"* 2>/dev/null | head -3

OUT=$(comshu_out ucx_mpi); rm -rf "$OUT"; mkdir -p "$OUT"

mpirun -np 2 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH \
  -x COMSHU_PROVIDER_PATH \
  "$COMSHU" run -p ucx -o "$OUT" -- "$APP"
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/19_ucx_mpi"
