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
#SBATCH --job-name=pcomm-19-ucx_mpi
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

# TWO NODES, ONE RANK EACH, ON PURPOSE. UCX picks shared memory for same-node
# pairs, and a shm-only run exercises the doors without ever touching the
# network — which would make this case green while saying nothing about the
# transport it exists to observe. Crossing a node boundary forces the rc/dc
# path.
MPI_BIN="$(dirname "$(command -v mpirun)")"
APP="$HOME/ucx_mpi_calls"
SRC="$PCOMM_ROOT/tests/hpc/probes/ucx_api_coverage/mpi_calls.c"

# Plain C and plain mpicc: this workload is deliberately NOT CUDA. The question
# is whether UCP doors are seen on MN5's MPI, and a GPU in the picture only adds
# a second provider's failure modes to a capture that is not about them.
if [ ! -x "$APP" ] || [ "$SRC" -nt "$APP" ]; then
  "$MPI_BIN/mpicc" -O2 -Wall -Wextra -Werror -o "$APP" "$SRC" \
    || { echo "!! mpicc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# UCX lives in the VERSIONED HPC-X tree, not under the comm_libs/mpi symlink
# that PATH resolves. Pointing at the symlink finds headers and no library; this
# path is the one the promoting probe verified. The provider .so was linked
# against it at build time (run_on_mn5.sh), so the runtime path has to agree.
UCX_LIB="$PCOMM_SDK/comm_libs/12.9/hpcx/hpcx-2.22.1/ucx/lib"
if [ ! -e "$UCX_LIB/libucp.so" ]; then
  echo "!! no libucp.so under $UCX_LIB" >&2
  echo "   (the provider was built against this path; a moved HPC-X needs both updated)" >&2
  exit 1
fi
export LD_LIBRARY_PATH="$UCX_LIB:$LD_LIBRARY_PATH"

# Say which UCX is actually in play, every run. The provider interposes public
# UCP symbols, so a second libucp on the path is not a warning, it is a capture
# that silently records nothing.
echo "=== ucx this run resolves ==="
ls -la "$UCX_LIB/libucp.so"* 2>/dev/null | head -3

OUT=$(pcomm_out ucx_mpi); rm -rf "$OUT"; mkdir -p "$OUT"

mpirun -np 2 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH \
  "$PCOMM" run -p ucx -o "$OUT" -- "$APP"
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/19_ucx_mpi"
