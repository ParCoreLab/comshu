#!/bin/bash
#SBATCH --job-name=comshu-vmmdoors
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:20:00
#SBATCH --output=vmmdoors-%j.out
#
# PROBE — which memory-sharing doors do NCCL and NVSHMEM actually use, and is
# the identity they pass between processes joinable?
#
# Not a suite case. It answers one question and then either becomes a record or
# gets deleted. See plans/PLAN-20260817-0705.
#
# WHY IT EXISTS. The previous attempt at object identity for VMM memory was
# built on an assumption about these doors — that cuMemCreate marks an
# application buffer — and cost three MN5 jobs to disprove: CUDA's own pooled
# allocator uses the same door, so case 04 went from 38 objects per rank to 818,
# 780 of them 2 MiB of pool backing. Route 2 rests on a different assumption,
# and this probe tests it BEFORE anything is built on it.
#
# THE QUESTIONS, in the order that matters:
#
#   1. Does either workload export/import a shareable handle at all? If NCCL
#      shares peer memory some other way (legacy cuIpc*, or a driver path with
#      no user-visible door), route 2 is dead for NCCL and the honest fallback
#      is naming the coalesced mapped arena instead.
#   2. What handle TYPE? A POSIX file descriptor is process-local — the fd
#      NUMBER is worthless as a join key — while a fabric handle is globally
#      meaningful. The two need completely different join designs.
#   3. For an fd, is (st_dev, st_ino) the same on both sides? That is the only
#      thing that could make an fd-based export joinable: two processes holding
#      the same kernel object see the same inode even with different fd numbers.
#      If the exporter's and importer's inodes differ, there is no join.
#   4. Is cuMemMap's `offset` ever non-zero? If it is always 0 the mapping is
#      the whole allocation and the record shrinks to a handle identity.
#
# READING THE RESULT: grep the .out for [comshu-vmm]. Lines are
#   create handle=H size=S dev=D type=T
#   export/import type=T fd=N dev=DEV ino=INO
#   map ptr=P size=S offset=O handle=H
# Compare the `ino` on an export against the `ino` on the matching import in a
# DIFFERENT rank's output. Same inode => joinable. Different => not.
set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

OUT="${COMSHU_VMM_OUT:-$HOME/comshu-suite/vmmdoors-${SLURM_JOB_ID:-local}}"
mkdir -p "$OUT"

# NCCL first: it is the workload whose objects are missing, and the one whose
# answer decides route 2.
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"
echo "=== NCCL (case 05's binary) ==="
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
  -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=0 -x NCCL_P2P_LEVEL=NVL \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  -x COMSHU_TRACE_VMM=1 \
  "$COMSHU" run -p p2p -o "$OUT/nccl" -- "$HOME/nccl_allreduce-li" 2>&1 | grep -E "comshu-vmm|ERROR" | head -400
echo "nccl exit=$?"

# NVSHMEM second, as the control: its heap IS shared between these same ranks,
# so whatever door it uses is a door that demonstrably works here. If NCCL shows
# nothing and NVSHMEM shows exports, the difference is NCCL's, not the probe's.
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$LD_LIBRARY_PATH"
echo "=== NVSHMEM (case 04's binary) ==="
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
  -x NVSHMEM_BOOTSTRAP=MPI -x COMSHU_KERNEL_NAME=put_to_next -x COMSHU_TRACE_VMM=1 \
  "$COMSHU" run -p p2p -o "$OUT/nvshmem" -- "$HOME/nvshmem_putget-li" 65536 3 2>&1 \
  | grep -E "comshu-vmm|ERROR" | head -400
echo "nvshmem exit=$?"

echo "=== summary ==="
echo "(counts per door are in the greps above; compare export/import inodes across ranks)"
