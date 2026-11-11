#!/bin/bash
# PROBE — Stage 2 of PLAN-20260803-2253-ibgda-device-initiated-capture.
#
# Not a suite case. It produces evidence for one decision, gets read once, and
# then either becomes providers/mlx5dv/ or gets deleted. It is kept out of
# tests/hpc/cases/ deliberately so run_suite never picks it up.
#
# Question: with NVSHMEM running IBGDA (GPU rings the NIC doorbell directly),
# does NVBit's existing instrumentation see the GPU stores that build WQEs?
#
#   YES -> option B is live: comshu can capture device-initiated IB passively,
#          for nearly nothing on a capture already running NVBit.
#   NO  -> option B is dead, and the answer is SigmaTrace's doorbell hijack.
#
# Same workload and binary as cases 04/07 (`putget`), and 8 PEs over 2 nodes so
# the ring crosses the node boundary twice — those two hops are the only IBGDA
# traffic in the run, which is what makes the join below unambiguous.
#
#SBATCH --job-name=comshu-probe-ibgda
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

PROBE_DIR="$COMSHU_ROOT/tests/hpc/probes/ibgda"
OUT="${COMSHU_OUT_ROOT:-$HOME/comshu-suite}/probe-ibgda-$SLURM_JOB_ID"
rm -rf "$OUT"; mkdir -p "$OUT/logs"

# ---- build the probe shim -------------------------------------------------
# common.sh exports NCCL_HOME/NVSHMEM_HOME but no CUDA_HOME; the SDK root it
# derives them from is the one thing that is guaranteed set.
CUDA_DIR="${CUDA_HOME:-$COMSHU_SDK/cuda}"
comshu_load_build
g++ -shared -fPIC -O2 -o "$OUT/devx_probe.so" "$PROBE_DIR/devx_probe.cpp" \
    -I"$CUDA_DIR/include" -L"$CUDA_DIR/lib64/stubs" -lcuda -ldl || {
  echo "PROBE ABORT: shim did not build (CUDA_DIR=$CUDA_DIR)"; exit 1; }

APP="$HOME/nvshmem_putget"
if [ ! -x "$APP" ]; then
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$COMSHU_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart
fi
comshu_load_runtime

# ---- force IBGDA ----------------------------------------------------------
# This is the exact inverse of what case 07 sets. 07 pins IBGDA *off* because
# "the IB half of this capture would silently come back empty rather than fail" —
# that silence is the hole this probe exists to measure.
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
export NVSHMEM_DEBUG=INFO
# NOT setting NVSHMEM_IB_DISABLE_DMABUF. Copying it from SigmaTrace is what
# killed job 44156479. IBGDA has to pin its control buffers for the NIC by one
# of two routes — DMABUF, or the nv_peer_mem/nvidia_peermem kernel module — and
# that flag disables the first. SigmaTrace sets it because *their* system lacks
# DMABUF support and needs the fallback; on MN5 the fallback is what is missing:
#
#   ibgda.cpp:nvshmemt_init:3577: neither nv_peer_mem, or nvidia_peermem
#   detected. Skipping transport.
#   init failed for transport: IBGDA
#
# so disabling DMABUF left IBGDA with no route at all. An environment flag
# carried across clusters without checking which half of an either/or it
# disables.

# ---- can this node do IBGDA at all? ---------------------------------------
# IBGDA needs one of two ways to let the NIC reach GPU memory. If neither is
# present the run cannot answer anything, and that is a fact about MN5 rather
# than about comshu — worth printing before the workload rather than inferring
# from a stack trace afterwards.
echo "=== IBGDA prerequisites on $(hostname) ==="
lsmod 2>/dev/null | grep -E "nvidia_peermem|nv_peer_mem" || echo "  peermem module: NOT LOADED"
ls /sys/kernel/mm/memory_peers/ 2>/dev/null || echo "  /sys/kernel/mm/memory_peers: absent"
ls /sys/class/infiniband/*/device/  2>/dev/null | grep -c dmabuf ||
  echo "  dmabuf: could not determine from sysfs (NVSHMEM_DEBUG will say)"

# ---- run ------------------------------------------------------------------
# -p p2p only: this asks what NVBit sees, and the ib provider has nothing to
# contribute on an IBGDA run (which is the entire finding being confirmed).
timeout 400 mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x COMSHU_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x NVSHMEM_REMOTE_TRANSPORT -x NVSHMEM_IB_ENABLE_IBGDA \
  -x NVSHMEM_IBGDA_NIC_HANDLER -x NVSHMEM_IB_DISABLE_DMABUF -x NVSHMEM_DEBUG \
  -x COMSHU_KERNEL_NAME=put_to_next \
  -x LD_PRELOAD="$OUT/devx_probe.so" \
  -x COMSHU_PROBE_LOG="$OUT/logs/probe.txt" \
  "$COMSHU" run -p p2p -o "$OUT/capture" -- "$APP" 65536 3
rc=$?; echo "mpirun exit=$rc"

# The shim writes one file per rank (probe.txt.<rank>) — mpirun does not expand
# %r, so the shim appends the rank itself. Job 44156479 had all eight ranks on
# one path and the log came back shredded mid-line, because fprintf+fflush is
# not atomic across processes.
echo "=== probe log (per rank) ==="
for f in "$OUT"/logs/probe.txt.*; do
  [ -e "$f" ] || continue
  echo "--- $f: $(wc -l < "$f") lines"
done

# The transport-init lines are what decide whether this run means anything, and
# they are the first thing to read on a failure — 44156479 died because IBGDA
# never initialised, which the probe log alone made look like the opposite.
echo "=== transport init ==="
grep -hE "transport|IBGDA|peermem|DMABUF|dmabuf" "$SLURM_SUBMIT_DIR"/*"$SLURM_JOB_ID".out 2>/dev/null |
  grep -viE "^\+" | sort -u | head -20

if [ $rc -ne 0 ]; then
  echo "PROBE: workload did not complete (rc=$rc; 124 = timed out)"
  echo "Running the analysis anyway — the probe log still says whether IBGDA"
  echo "engaged, and on a failed run that is the whole question."
  python3 "$PROBE_DIR/analyze.py" "$OUT/logs" "$OUT/capture.csv"
  exit 1
fi

"$COMSHU" export "$OUT/capture" --csv "$OUT/capture.csv"

# ---- the join -------------------------------------------------------------
echo "=== ANSWER ==="
python3 "$PROBE_DIR/analyze.py" "$OUT/logs" "$OUT/capture.csv"
