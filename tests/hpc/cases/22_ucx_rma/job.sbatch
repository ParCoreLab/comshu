#!/bin/bash
# Case 22 — ucx_rma: the one-sided and legacy DOORS, and the keys they name.
#
# WHY THIS CASE EXISTS, given 21_ucx_deep already issues Put and Get. That case
# is about which TRANSPORT protocol UCX picks by size, and its assertions are
# pinned to the thresholds it sets. This one is about which UCP DOOR MPI reaches,
# which is a different axis and a colder one: PLAN-20260815-1540's *Future work
# 2* records that the `_nbi` RMA doors, the three atomic doors and the legacy
# synchronous tagged door were all hooked on a STATIC ELF SURVEY of MN5's HPC-X
# and never once observed firing. That is good evidence the code path exists and
# no evidence at all that it is taken.
#
# And every one of those doors names a remote key. `ucx_rma.rkey` and
# `ucx_atomic.rkey` intern a `ucp_rkey_h`, and until `ucx_rkey` (626) existed
# nothing recorded one being created — so the handle on every one-sided row in
# every capture this project has ever taken joined to NOTHING. The survey behind
# that record was run on this machine's own libraries:
#
#   mca_osc_ucx    MPI one-sided   ucp_ep_rkey_unpack, ucp_rkey_pack,
#                                  ucp_rkey_destroy, ucp_rkey_buffer_release
#   mca_spml_ucx   OpenSHMEM       the same four, plus ucp_rkey_compare
#   libnccl-net    NCCL plugin     the same four
#   mca_pml_ucx    MPI p2p         none — point-to-point unpacks no keys
#
# so MPI RMA is exactly the workload that makes those doors fire.
#
# ONE MORE THING IT CLOSES: `comshu run -p ucx,ib` in ONE SHARED PASS. The UCX
# plan's definition of done requires it and it had never been executed — the
# smoke job that ran used `-p ucx` with the verbs provider compiled out. Two
# hook-based providers in one process is what the shared engine exists for, and
# a case that runs it is the only thing that proves the two do not fight over
# dlsym.
#
#   sbatch tests/hpc/cases/22_ucx_rma/job.sbatch
#
#SBATCH --job-name=comshu-22-ucx_rma
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
# reaching the rc/dc transports. It matters more here than there — a shared
# memory endpoint may not unpack a remote key at all, which would leave the
# record this case exists for empty and look like a tracer defect.
MPI_BIN="$(dirname "$(command -v mpirun)")"
APP="$HOME/rma_doors"
SRC="$COMSHU_ROOT/tests/hpc/probes/ucx_rma_doors/rma_doors.c"

if [ ! -x "$APP" ] || [ "$SRC" -nt "$APP" ]; then
  "$MPI_BIN/mpicc" -O2 -Wall -Wextra -Werror -o "$APP" "$SRC" \
    || { echo "!! mpicc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# The UCX tree belonging to the MPI THAT WILL ACTUALLY RUN — case 21's reason,
# and the same one definition. The provider interposes public UCP symbols, so
# build against one libucp and run against another and the interposition
# silently does not engage.
UCX_HOME_RESOLVED="$(comshu_find_ucx)" || {
  echo "!! no HPC-X UCX tree found beside this MPI ($(command -v mpirun))" >&2
  exit 1; }
UCX_LIB="$UCX_HOME_RESOLVED/lib"
[ -e "$UCX_LIB/libucp.so" ] || { echo "!! no libucp.so under $UCX_LIB" >&2; exit 1; }
export LD_LIBRARY_PATH="$UCX_LIB:$LD_LIBRARY_PATH"

echo "=== ucx this run resolves ==="
ls -la "$UCX_LIB/libucp.so"* 2>/dev/null | head -3
"$UCX_HOME_RESOLVED/bin/ucx_info" -v 2>/dev/null | head -3

# THE SIZES. Small and deliberately below every rendezvous threshold: this case
# is not about protocol selection — 21_ucx_deep owns that question and pins
# thresholds to ask it. Here the only requirement is that the operations
# COMPLETE and reach their doors, so the defaults are left alone and the sizes
# stay well inside them.
export RMA_ITERS=10 RMA_BYTES=4096 WIN_BYTES=65536 SSEND_ITERS=10 SSEND_BYTES=64

# --mca osc ucx is LOAD-BEARING, and case 21 learned it the expensive way:
# MPI_Put/MPI_Get reach UCX's one-sided doors only through the UCX one-sided
# component. With osc/rdma or osc/pt2pt they become active messages, no rkey is
# ever unpacked, and this case's central table would be empty for a reason that
# has nothing to do with the provider.
OUT="$(comshu_out ucx_rma)"
rm -rf "$OUT"; mkdir -p "$OUT"

# -p ucx,ib — the shared pass. Both are hook-based and therefore non-exclusive,
# so they co-run in ONE process rather than forcing a replay; that is precisely
# the arrangement the shared engine was built for and the one never yet run.
mpirun -np 2 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
  -x RMA_ITERS -x RMA_BYTES -x WIN_BYTES -x SSEND_ITERS -x SSEND_BYTES \
  --mca osc ucx \
  "$COMSHU" run -p ucx,ib -o "$OUT" -- "$APP"
echo "mpirun exit=$?"

echo "=== captured streams ($OUT) ==="
"$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"

# Printed for the record, because "which door did MPI actually take" is the
# question this case exists to answer and a human reading the job output should
# not have to run SQL to see the answer.
echo "=== the doors this stack took ==="
if [ -f "$OUT.csv/ucx_rma.csv" ]; then
  echo "--- ucx_rma by door"; tail -n +2 "$OUT.csv/ucx_rma.csv" | cut -d, -f4 | sort | uniq -c
fi
if [ -f "$OUT.csv/ucx_atomic.csv" ]; then
  echo "--- ucx_atomic by op/door"; tail -n +2 "$OUT.csv/ucx_atomic.csv" | cut -d, -f3,4 | sort | uniq -c
fi
if [ -f "$OUT.csv/ucx_tag_send.csv" ]; then
  echo "--- ucx_tag_send by door"; tail -n +2 "$OUT.csv/ucx_tag_send.csv" | cut -d, -f3 | sort | uniq -c
fi
if [ -f "$OUT.csv/ucx_rkey.csv" ]; then
  echo "--- ucx_rkey rows"; tail -n +2 "$OUT.csv/ucx_rkey.csv" | wc -l
  echo "--- ucx_rkey_pack rows"; tail -n +2 "$OUT.csv/ucx_rkey_pack.csv" 2>/dev/null | wc -l
fi

# THE LAST LINE, and it is not optional. run_suite.sh aggregates the board from
# a `result.json` that each case's own job is responsible for dropping in its
# output directory — nothing runs the assertions on a case's behalf. Omitting
# this call is what made job 45177421 come back as `22_ucx_rma ---- no result`
# against a capture that was complete and correct, which is the same queue slot
# 21_ucx_deep's own comment records losing to the sibling mistake of naming the
# output directory for the arm instead of the case.
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/22_ucx_rma"
