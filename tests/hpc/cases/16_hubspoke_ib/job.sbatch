#!/bin/bash
# Case 16 — hubspoke_ib: NVSHMEM star, spokes push to rank 0 and get an eighth back, ONE GPU PER NODE
#
# Shares tests/hpc/asym_patterns.cu with the other two asymmetric-pattern cases;
# only the pattern argument differs. See that file for why these exist — briefly,
# every capture in the corpus was a ring or a pair, so every comm matrix was
# symmetric and every object heatmap was flat, and neither of those could be
# told apart from a bug.
#
#   sbatch tests/hpc/cases/16_hubspoke_ib/job.sbatch
#
#SBATCH --job-name=comshu-16-hubspoke_ib
#SBATCH --account=etur02
# acc_debug. This was acc_ehpc, on the reasoning that debug's one-job limit
# serialises every experiment behind the last one. Measured on 2026-08-17, that
# trade is backwards: job 44715122 (case 18, ehpc) sat PENDING for 80 minutes
# with Reason=Priority, and the identical job resubmitted to acc_debug
# (44718913) started in 18 seconds and ran in 32. `sprio` says why — the QoS
# factor is 100x on debug (priority weight 10000 vs 100), which took the job
# from 30830 to 129303. Serialising three half-minute jobs costs less than one
# ehpc queue wait.
#
# 4 nodes x 15 min is inside debug's ceiling (node=8, MaxWall 02:00:00), so
# this fits without a DenyOnLimit rejection.
#
# The one-job limit is per-QoS, not per-user: MaxJobsPU=1 belongs to acc_debug
# alone, while ehpc allows 366. So a debug job and an ehpc job coexist, and it
# is only these three cases that now serialise against the suite's other
# acc_debug jobs.
#SBATCH --qos=acc_debug
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

# Compute capability from the device rather than hardcoded, because these three
# cases are meant to run on MN5's H100s (sm_90) AND on the A100 box (sm_80),
# where run_on_a100.sh executes this file directly. A fixed -gencode is why the
# older cases are MN5-only.
# COMSHU_SM, not CC: `CC` is the C compiler variable, and comshu_load_build below
# loads modules that export it. The first version used CC and was clobbered
# BETWEEN its two uses — the binary was named asym_patterns-sm90 (correct, read
# before the modules loaded) while -gencode became
# arch=compute_/apps/.../bin/nvc (read after). nvcc rejected it and the case
# then ran a binary that did not exist, exit 127.
COMSHU_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
COMSHU_SM=${COMSHU_SM:-90}
APP="$HOME/asym_patterns-li-sm$COMSHU_SM"

# NVSHMEM_HOME is resolved by globbing the SDKs and taking the newest, so it can
# name a directory whose layout differs from the one the modules provide. If the
# host library is not where we are about to point -L, the link silently falls
# back to whatever the module put on the default search path and the mismatch
# only surfaces as an undefined symbol at startup — which is how job 44457910
# was spent. Check it here, where the message can say which directory.
if [ ! -e "$NVSHMEM_HOME/lib/libnvshmem_host.so" ]; then
  echo "!! no libnvshmem_host.so under $NVSHMEM_HOME/lib" >&2
  ls "$NVSHMEM_HOME/lib" 2>/dev/null | head -20 >&2
  exit 1
fi

# One binary per arch for all three cases, so the three captures differ only in
# the pattern and never in the build. The arch is in the name because a cached
# binary from the other machine is not a build error, it is a silent wrong
# answer.
if [ ! -x "$APP" ] || [ "$COMSHU_ROOT/tests/hpc/asym_patterns.cu" -nt "$APP" ]; then
  comshu_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -rdc=true \
    -gencode arch=compute_$COMSHU_SM,code=sm_$COMSHU_SM -Wno-deprecated-gpu-targets \
    "$COMSHU_ROOT/tests/hpc/asym_patterns.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -Xlinker --disable-new-dtags -Xlinker -rpath -Xlinker "$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart \
    || { echo "!! nvcc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# Unconditionally, AFTER the build block, so the module state does not depend on
# whether we happened to build. When the binary was stale the block ran
# comshu_load_build and then reloaded the runtime, leaving a different PATH than
# when the binary was fresh and the block was skipped entirely. Case 16 built and
# worked; 17 and 18 reused its binary and died with Open MPI "not enough slots"
# on an identical 4-node allocation, because they got a different mpirun.
# A run must not depend on whether a previous run left a binary behind.
comshu_load_runtime
echo "=== mpirun: $(command -v mpirun)"

# WHICH NVSHMEM this binary actually resolves, printed every run.
#
# Job 44457910 died at startup with "undefined symbol:
# nvshmem_selected_device_transport, version NVSHMEM". NVSHMEM_HOME is resolved
# by globbing the SDKs and taking the newest (26.5 on MN5), while
# comshu_load_runtime does `module load nvidia-hpc-sdk/25.7` — so the headers and
# -L can come from one NVSHMEM and the loader can find another. Case 04 does not
# hit this only because its binary is cached from before the two diverged.
#
# -rpath above pins the binary to the NVSHMEM it was linked against, which is
# the fix. This line is the evidence: a version mismatch should be one grep in
# the log, not another job.
# LD_LIBRARY_PATH is exported HERE, after the build block, and that ordering is
# the whole bug. It used to sit at the top, before the build — but the build
# block ends with comshu_load_runtime, which re-runs `module load
# nvidia-hpc-sdk/25.7` and PREPENDS 25.7's paths, putting them ahead of the
# NVSHMEM_HOME (26.5) the binary was just linked against. Only 26.5 defines
# nvshmem_selected_device_transport, so the loader found 25.7's libnvshmem_host
# first and the process died at startup (job 44457910).
#
# Case 04 has the same shape and survives only because its binary is cached, so
# its build block never runs and nothing re-prepends. Its next rebuild would
# have hit this — a bug that hides until the day someone changes the workload.
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

echo "=== nvshmem this binary links against ==="
ldd "$APP" 2>/dev/null | grep -i nvshmem || echo "  (none reported by ldd)"

OUT=$(comshu_out hubspoke_ib); rm -rf "$OUT"; mkdir -p "$OUT"

# ONE RANK PER NODE, and that is the whole point of this case. On one node
# NVSHMEM routes every put over NVLink and the NIC carries no data at all, so an
# ibverbs capture would hold bootstrap traffic and nothing else. Measured on case
# 07 (8 PEs, 2 nodes): only the 2 ring hops that crossed the node boundary
# appeared as verbs data; the other 6 stayed on NVLink and were invisible here.
# With one GPU per node every edge crosses the NIC and the whole pattern is
# visible in rank space.
#
# -p ib only. No COMSHU_KERNEL_NAME: that filter is an NVBit concept and there is
# no NVBit in this configuration. Deliberately NOT running p2p alongside — it has
# an open illegal-memory-access bug (PLAN-20260810-0854) that poisons the context
# and costs the capture, and these three exist to be usable while that is open.
mpirun -np 4 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH \
  -x COMSHU_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  "$COMSHU" run -p ib -o "$OUT" -- "$APP" hub 4096 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$COMSHU_ROOT/tests/hpc/cases/16_hubspoke_ib"
