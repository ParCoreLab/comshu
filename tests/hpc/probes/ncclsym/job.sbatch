#!/bin/bash
#SBATCH --job-name=comshu-ncclsym
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:20:00
#SBATCH --output=ncclsym-%j.out
#
# PROBE — see probe.cu for the two questions. Not a suite case.
#
# It exists because the two routes left for NCCL object attribution both rest on
# an assumption that can be checked in one job:
#   route 1 (library boundary) needs NCCL's symmetric window to be symmetric in
#           the VA space, not just in NCCL's bookkeeping;
#   route 2 (handle join) needs fabric handles, since the POSIX-fd handle this
#           stack uses fstat'd to one driver-node inode for all 48 exports and
#           imports (job 44705498).
set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

APP="$HOME/ncclsym_probe"
SRC="$COMSHU_ROOT/tests/hpc/probes/ncclsym/probe.cu"
if [ ! -x "$APP" ] || [ "$SRC" -nt "$APP" ]; then
  comshu_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -gencode arch=compute_90,code=sm_90 \
    -Wno-deprecated-gpu-targets "$SRC" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart -lcuda \
    || { echo "!! nvcc failed" >&2; exit 1; }
  comshu_load_runtime
fi

export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"
echo "=== nccl in play ==="; ldd "$APP" | grep -i nccl

# Bare first: the address question is about NCCL, not about comshu, and a tracer
# in the picture is one more thing that could be blamed for the answer.
echo "=== BARE (no tracer) ==="
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH -x NCCL_DEBUG=WARN "$APP"

# Then under the tracer, so the doors are visible alongside the addresses.
OUT="${COMSHU_OUT_ROOT:-$HOME/comshu-suite}/ncclsym-${SLURM_JOB_ID:-local}"
rm -rf "$OUT"; mkdir -p "$OUT"
echo "=== UNDER the tracer, p2p provider, with door tracing ==="
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
  -x NCCL_DEBUG=WARN -x COMSHU_TRACE_VMM=1 \
  "$COMSHU" run -p p2p -o "$OUT" -- "$APP" 2>&1 | grep -E "ncclsym|comshu-vmm" | head -200

echo "=== captured streams ==="; "$COMSHU" read "$OUT" 2>&1 | head -20

echo "=== THE ANSWER ==="
echo "fabric_supported: same on every rank? (1 = fabric handles usable here)"
echo "ncclMemAlloc addresses: IDENTICAL across ranks => symmetric VA => comshu's"
echo "  existing (base,size) translation works; DIFFERENT => it does not."
