#!/bin/bash
# PROBE — WHICH LIBRARY calls each memory-registration door?
#
# Not a suite case. It answers one question and then either becomes a fix or
# gets deleted. See plans/PLAN-20260806-2320-dlvsym-interposer-not-observed.
#
# entity_close_accounted reports 33 closes per rank naming regions nothing
# opened, all opener=ib_mem_region (job 44363299). Both registration hooks
# record their region unconditionally on success, so an unmatched close means
# the registration never entered ibverbs_provider.cpp — while the matching
# ibv_dereg_mr did.
#
# The hunt for that door assumed NCCL, because NCCL's source resolves ibv_*
# through dlvsym. Job 44368260 killed that assumption: dlvsym IS entered on all
# 8 ranks (so the interposer works) and never once with an ibv_* name, and
# neither is dlsym. Nothing in that process resolves ibverbs dynamically at all.
# Six hypotheses have now died; this stops asking "which symbol" and asks "which
# caller", which no symbol tool and no resolution trace could answer.
#
# PCOMM_TRACE_MR_CALLERS=1 dladdr's __builtin_return_address(0) in each of the
# three MR interposers, printing one line per distinct (door, calling library).
# Read it as a set difference:
#
#   a library under BOTH reg and dereg  -> registers through a door we hook
#   a library under dereg ONLY          -> THE MISSING DOOR, and now named
#
# The obvious unexamined occupant is OpenMPI/UCX: this run has been read as pure
# NCCL throughout, and MPI uses InfiniBand too.
#
# POSITIVE CONTROL: any [pcomm-mrcaller] line at all. If none appear the
# diagnostic did not run and nothing below is a finding — an earlier probe here
# mis-stated its own control, and another reported a false positive because
# grep -c matched the filename it was searching rather than a line in it.
#
#SBATCH --job-name=pcomm-probe-mrcaller
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_allreduce"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-mrcaller-${SLURM_JOB_ID:-local}"
rm -rf "$OUT"; mkdir -p "$OUT"

# Same configuration as case 02 — same providers, same NCCL pinning — because a
# probe that changes the workload cannot be compared against the board it is
# explaining. Only the iteration count drops: symbol resolution happens once, at
# NCCL init, so 2 iterations observe exactly what 20 would.
mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x PCOMM_TRACE_DLSYM=1 -x PCOMM_TRACE_MR_CALLERS=1 \
  -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=1 \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" -p ib -o "$OUT" -- "$APP" 8388608 2 2>&1 | tee "$OUT/run.log"
echo "mpirun exit=${PIPESTATUS[0]}"

echo "=== POSITIVE CONTROL: did each diagnostic run at all? ==="
grep -h 'first entry' "$OUT/run.log" | sort -u
printf 'mrcaller lines: %s
' "$(grep -c 'pcomm-mrcaller' "$OUT/run.log")"

echo "=== THE ANSWER: which library calls which registration door ==="
grep -h 'pcomm-mrcaller' "$OUT/run.log" | sort -u

echo "=== set difference: callers of dereg that never call a reg door ==="
awk '/pcomm-mrcaller/ {door=$2; who=$4; if (door=="dereg_mr") d[who]=1; else r[who]=1}
     END {n=0; for (w in d) if (!(w in r)) {print "  DEREG-ONLY: " w; n++}
          if (n==0) print "  (none — every deregistering library also registers through a hooked door)"}' \
  "$OUT/run.log"

# The narrowed question. The per-door table above cannot tell "registered by a
# different library" from "registered through a door we do not hook"; following
# the individual mr can, and job 44368667 showed why that distinction matters —
# libuct_ib_mlx5 looked like the missing door and is very likely just closing
# what libuct_ib opened.
echo "=== UNMATCHED: closes of regions no hooked door opened ==="
printf 'unmatched lines: %s\n' "$(grep -c 'pcomm-unmatched' "$OUT/run.log")"
grep -h 'pcomm-unmatched' "$OUT/run.log" | sed 's/.*<- //' | sort | uniq -c | sort -rn

echo "=== their sizes — a 1GB region is a heap, a 4KB one is a control buffer ==="
grep -h 'pcomm-unmatched' "$OUT/run.log" | grep -o 'len=[0-9]*' | sort -n -t= -k2 | uniq -c

echo "=== first few in full ==="
grep -h 'pcomm-unmatched' "$OUT/run.log" | head -10

echo "=== dlsym/dlvsym entry, for continuity with job 44368260 ==="
grep -h 'pcomm-entry' "$OUT/run.log" | grep -v 'first entry' | sort -u
