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
#   YES -> option B is live: pcomm can capture device-initiated IB passively,
#          for nearly nothing on a capture already running NVBit.
#   NO  -> option B is dead, and the answer is SigmaTrace's doorbell hijack.
#
# Same workload and binary as cases 04/07 (`putget`), and 8 PEs over 2 nodes so
# the ring crosses the node boundary twice — those two hops are the only IBGDA
# traffic in the run, which is what makes the join below unambiguous.
#
#SBATCH --job-name=pcomm-probe-ibgda
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

PROBE_DIR="$PCOMM_ROOT/tests/hpc/probes/ibgda"
OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-ibgda-$SLURM_JOB_ID"
rm -rf "$OUT"; mkdir -p "$OUT/logs"

# ---- build the probe shim -------------------------------------------------
pcomm_load_build
g++ -shared -fPIC -O2 -o "$OUT/devx_probe.so" "$PROBE_DIR/devx_probe.cpp" \
    -I"$CUDA_HOME/include" -L"$CUDA_HOME/lib64/stubs" -lcuda -ldl || {
  echo "PROBE ABORT: shim did not build"; exit 1; }

APP="$HOME/nvshmem_putget"
if [ ! -x "$APP" ]; then
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart
fi
pcomm_load_runtime

# ---- force IBGDA ----------------------------------------------------------
# This is the exact inverse of what case 07 sets. 07 pins IBGDA *off* because
# "the IB half of this capture would silently come back empty rather than fail" —
# that silence is the hole this probe exists to measure.
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
# SigmaTrace sets this; without it NVSHMEM logs "system does not support
# registering the NIC control buffers with DMABUF" and may fall back.
export NVSHMEM_IB_DISABLE_DMABUF=1
export NVSHMEM_DEBUG=INFO

# ---- run ------------------------------------------------------------------
# -p p2p only: this asks what NVBit sees, and the ib provider has nothing to
# contribute on an IBGDA run (which is the entire finding being confirmed).
timeout 400 mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x NVSHMEM_REMOTE_TRANSPORT -x NVSHMEM_IB_ENABLE_IBGDA \
  -x NVSHMEM_IBGDA_NIC_HANDLER -x NVSHMEM_IB_DISABLE_DMABUF -x NVSHMEM_DEBUG \
  -x PCOMM_KERNEL_NAME=put_to_next \
  -x LD_PRELOAD="$OUT/devx_probe.so" \
  -x PCOMM_PROBE_LOG="$OUT/logs/probe.txt" \
  "$PCOMM" -p p2p -o "$OUT/capture" -- "$APP" 65536 3
rc=$?; echo "mpirun exit=$rc"

# LD_PRELOAD is process-wide, so all 8 ranks append to one log unless the
# launcher gives each its own name. mpirun does not expand %r, so ranks share
# the file; entries are flushed per line, which is enough for set arithmetic.
echo "=== probe log ==="; cat "$OUT/logs/probe.txt" 2>/dev/null | sort | uniq -c | sort -rn | head -40

if [ $rc -ne 0 ]; then
  echo "PROBE INCONCLUSIVE: workload did not complete (rc=$rc; 124 = timed out)"
  echo "A hang here is itself a finding — case 07's history is instrumenting"
  echo "NVSHMEM-internal device code wedging the transport."
  exit 1
fi

"$PCOMM_READ" "$OUT/capture" --csv "$OUT/capture.csv"

# ---- the join -------------------------------------------------------------
echo "=== ANSWER ==="
python3 "$PROBE_DIR/analyze.py" "$OUT/logs/probe.txt" "$OUT/capture.csv"
